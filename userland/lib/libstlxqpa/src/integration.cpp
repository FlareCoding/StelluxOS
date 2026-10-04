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

QStelluxScreen::QStelluxScreen(const QSize& size)
    : m_geometry(QPoint(0, 0), size)
    , m_cursor(std::make_unique<QStelluxCursor>()) {
}

QRect QStelluxScreen::geometry() const {
    return m_geometry;
}

int QStelluxScreen::depth() const {
    return SCREEN_DEPTH;
}

QImage::Format QStelluxScreen::format() const {
    return QImage::Format_RGB32;
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

    uint32_t width = 0;
    uint32_t height = 0;
    stlxwin_screen_size(m_connection, &width, &height);

    m_screen = new QStelluxScreen(QSize(static_cast<int>(width), static_cast<int>(height)));
    QWindowSystemInterface::handleScreenAdded(m_screen);
}

QStelluxIntegration::~QStelluxIntegration() {
    delete m_notifier;

    if (m_screen) {
        QWindowSystemInterface::handleScreenRemoved(m_screen);
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

        QStelluxWindow* window = m_windows.value(event.window);
        if (window) {
            window->handle_event(event);
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
