#include "integration.h"
#include "backing_store.h"
#include "clipboard.h"
#include "font_database.h"
#include "services.h"
#include "theme.h"
#include "window.h"

#include <QCoreApplication>
#include <QCursor>
#include <QSocketNotifier>
#include <QWindow>
#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <qpa/qwindowsysteminterface.h>

static const int SCREEN_DEPTH = 32;

// stlxdm's 0 for an unknown refresh stays 0, which Qt takes for 60 Hz
static const qreal UNKNOWN_REFRESH_RATE = 0;
static const qreal MILLIHERTZ_PER_HERTZ = 1000;

static QStelluxIntegration* s_instance = nullptr;

static stlxwin_cursor to_stlxwin_cursor(Qt::CursorShape shape) {
    switch (shape) {
        case Qt::IBeamCursor:
            return STLXWIN_CURSOR_IBEAM;
        case Qt::PointingHandCursor:
            return STLXWIN_CURSOR_HAND;
        case Qt::SizeHorCursor:
        case Qt::SplitHCursor:
            return STLXWIN_CURSOR_RESIZE_H;
        case Qt::SizeVerCursor:
        case Qt::SplitVCursor:
            return STLXWIN_CURSOR_RESIZE_V;
        case Qt::SizeFDiagCursor:
            return STLXWIN_CURSOR_RESIZE_NWSE;
        case Qt::SizeBDiagCursor:
            return STLXWIN_CURSOR_RESIZE_NESW;
        case Qt::BlankCursor:
            return STLXWIN_CURSOR_NONE;
        default:
            return STLXWIN_CURSOR_ARROW;
    }
}

void QStelluxCursor::changeCursor(QCursor* cursor, QWindow* window) {
    if (!window || !window->handle()) {
        return;
    }

    auto* platform_window = static_cast<QStelluxWindow*>(window->handle());
    if (!platform_window->handle()) {
        return;
    }

    Qt::CursorShape shape = cursor ? cursor->shape() : Qt::ArrowCursor;
    stlxwin_window_set_cursor(platform_window->handle(), to_stlxwin_cursor(shape));
}

QStelluxScreen::QStelluxScreen(uint32_t output, const QRect& geometry, const QRect& available_geometry,
                               qreal refresh_rate)
    : m_output(output)
    , m_geometry(geometry)
    , m_available_geometry(available_geometry)
    , m_refresh_rate(refresh_rate)
    , m_cursor(std::make_unique<QStelluxCursor>()) {
}

uint32_t QStelluxScreen::output() const {
    return m_output;
}

void QStelluxScreen::update(const QRect& geometry, const QRect& available_geometry, qreal refresh_rate) {
    if (geometry != m_geometry || available_geometry != m_available_geometry) {
        m_geometry = geometry;
        m_available_geometry = available_geometry;
        QWindowSystemInterface::handleScreenGeometryChange(screen(), geometry, available_geometry);
    }

    if (refresh_rate != m_refresh_rate) {
        m_refresh_rate = refresh_rate;
        QWindowSystemInterface::handleScreenRefreshRateChange(screen(), refresh_rate);
    }
}

QRect QStelluxScreen::geometry() const {
    return m_geometry;
}

QRect QStelluxScreen::availableGeometry() const {
    return m_available_geometry;
}

int QStelluxScreen::depth() const {
    return SCREEN_DEPTH;
}

QImage::Format QStelluxScreen::format() const {
    return QImage::Format_RGB32;
}

QDpi QStelluxScreen::logicalDpi() const {
    return QDpi(SCREEN_LOGICAL_DPI, SCREEN_LOGICAL_DPI);
}

qreal QStelluxScreen::refreshRate() const {
    return m_refresh_rate;
}

QList<QPlatformScreen*> QStelluxScreen::virtualSiblings() const {
    return QStelluxIntegration::instance()->screens();
}

QPlatformCursor* QStelluxScreen::cursor() const {
    return m_cursor.get();
}

QStelluxIntegration::QStelluxIntegration() {
    s_instance = this;

    m_connection = stlxwin_connect(QCoreApplication::applicationName().toUtf8().constData());
    if (!m_connection) {
        return;
    }

    update_screens();
    if (!m_screens.isEmpty()) {
        return;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    stlxwin_screen_size(m_connection, &width, &height);

    QRect geometry(0, 0, static_cast<int>(width), static_cast<int>(height));
    m_screens.append(new QStelluxScreen(0, geometry, geometry, UNKNOWN_REFRESH_RATE));
    QWindowSystemInterface::handleScreenAdded(m_screens.last());
}

QStelluxIntegration::~QStelluxIntegration() {
    delete m_notifier;

    while (!m_screens.isEmpty()) {
        QWindowSystemInterface::handleScreenRemoved(m_screens.takeLast());
    }

    if (m_connection) {
        stlxwin_disconnect(m_connection);
    }

    s_instance = nullptr;
}

QStelluxIntegration* QStelluxIntegration::instance() {
    return s_instance;
}

bool QStelluxIntegration::is_connected() const {
    return m_connection != nullptr;
}

stlxwin_conn* QStelluxIntegration::connection() const {
    return m_connection;
}

void QStelluxIntegration::add_window(stlxwin_window* handle, QStelluxWindow* window) {
    m_windows.insert(handle, window);
}

void QStelluxIntegration::remove_window(stlxwin_window* handle) {
    m_windows.remove(handle);
}

QList<QStelluxWindow*> QStelluxIntegration::windows() const {
    return m_windows.values();
}

QList<QPlatformScreen*> QStelluxIntegration::screens() const {
    return QList<QPlatformScreen*>(m_screens.cbegin(), m_screens.cend());
}

QStelluxScreen* QStelluxIntegration::screen_of(uint32_t output) const {
    for (QStelluxScreen* screen : m_screens) {
        if (screen->output() == output) {
            return screen;
        }
    }

    return nullptr;
}

void QStelluxIntegration::initialize() {
    m_notifier = new QSocketNotifier(stlxwin_conn_fd(m_connection), QSocketNotifier::Read);
    QObject::connect(m_notifier, &QSocketNotifier::activated, m_notifier, [this] { read_connection(); });
}

// A blocking begin_frame keeps reading the connection, so events can be queued
// with the fd already drained and no notifier wakeup left to deliver them
void QStelluxIntegration::schedule_queued_events() {
    QMetaObject::invokeMethod(m_notifier, [this] { dispatch_queued_events(); }, Qt::QueuedConnection);
}

void QStelluxIntegration::read_connection() {
    if (stlxwin_dispatch(m_connection) < 0) {
        QCoreApplication::quit();
        return;
    }

    dispatch_queued_events();
}

void QStelluxIntegration::dispatch_queued_events() {
    stlxwin_event event;
    while (stlxwin_next_event(m_connection, &event)) {
        if (event.type == STLXWIN_EVT_DISCONNECTED) {
            QCoreApplication::quit();
            return;
        }

        if (event.type == STLXWIN_EVT_OUTPUTS) {
            update_screens();
            continue;
        }

        if (event.type == STLXWIN_EVT_CLIPBOARD) {
            static_cast<QStelluxClipboard*>(m_clipboard.get())->changed();
            continue;
        }

        QStelluxWindow* window = m_windows.value(event.window);
        if (window) {
            window->handle_event(event);
        }
    }
}

void QStelluxIntegration::update_screens() {
    stlxwin_output outputs[STLXWIN_OUTPUTS_MAX];
    long count = stlxwin_outputs_get(m_connection, nullptr, outputs, STLXWIN_OUTPUTS_MAX);

    QList<QStelluxScreen*> listed;
    QStelluxScreen* main = nullptr;
    for (long i = 0; i < qMin(count, static_cast<long>(STLXWIN_OUTPUTS_MAX)); i++) {
        const stlxwin_output& output = outputs[i];
        QRect geometry(output.x, output.y, static_cast<int>(output.width), static_cast<int>(output.height));
        QRect available_geometry(output.work.x, output.work.y, output.work.w, output.work.h);
        qreal refresh_rate = static_cast<qreal>(output.refresh_millihertz) / MILLIHERTZ_PER_HERTZ;

        QStelluxScreen* screen = screen_of(output.output);
        if (screen) {
            screen->update(geometry, available_geometry, refresh_rate);
        } else {
            screen = new QStelluxScreen(output.output, geometry, available_geometry, refresh_rate);
            m_screens.append(screen);
            QWindowSystemInterface::handleScreenAdded(screen);
        }

        listed.append(screen);
        if (output.flags & STLXWIN_DESKTOP_MAIN) {
            main = screen;
        }
    }

    if (main) {
        QWindowSystemInterface::handlePrimaryScreenChanged(main);
    }

    const QList<QStelluxScreen*> previous = m_screens;
    for (QStelluxScreen* screen : previous) {
        if (!listed.contains(screen)) {
            m_screens.removeOne(screen);
            QWindowSystemInterface::handleScreenRemoved(screen);
        }
    }
}

bool QStelluxIntegration::hasCapability(Capability capability) const {
    switch (capability) {
        case ThreadedPixmaps:
        case MultipleWindows:
        case NonFullScreenWindows:
            return true;
        default:
            return QPlatformIntegration::hasCapability(capability);
    }
}

QPlatformWindow* QStelluxIntegration::createPlatformWindow(QWindow* window) const {
    return new QStelluxWindow(window);
}

QPlatformBackingStore* QStelluxIntegration::createPlatformBackingStore(QWindow* window) const {
    return new QStelluxBackingStore(window);
}

QAbstractEventDispatcher* QStelluxIntegration::createEventDispatcher() const {
    return createUnixEventDispatcher();
}

QPlatformFontDatabase* QStelluxIntegration::fontDatabase() const {
    if (!m_font_database) {
        m_font_database = std::make_unique<QStelluxFontDatabase>();
    }

    return m_font_database.get();
}

QPlatformClipboard* QStelluxIntegration::clipboard() const {
    if (!m_clipboard) {
        m_clipboard = std::make_unique<QStelluxClipboard>(m_connection);
    }

    return m_clipboard.get();
}

QPlatformServices* QStelluxIntegration::services() const {
    if (!m_services) {
        m_services = std::make_unique<QStelluxServices>();
    }

    return m_services.get();
}

QStringList QStelluxIntegration::themeNames() const {
    return { QString::fromLatin1(QStelluxTheme::NAME) };
}

QPlatformTheme* QStelluxIntegration::createPlatformTheme(const QString& name) const {
    if (name == QLatin1String(QStelluxTheme::NAME)) {
        return new QStelluxTheme;
    }

    return QPlatformIntegration::createPlatformTheme(name);
}
