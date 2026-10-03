#include "window.h"
#include "integration.h"

#include <QGuiApplication>
#include <QHash>
#include <QVarLengthArray>
#include <QWheelEvent>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

#include <cstring>

static const QSize DEFAULT_WINDOW_SIZE(640, 480);
static const int BYTES_PER_PIXEL = 4;
static const int FIRST_PRINTABLE = 0x20;
static const int LAST_PRINTABLE = 0x7e;

// USB HID keyboard page usages for the keys that produce no printable character
enum hid_usage : uint16_t {
    HID_ENTER = 0x28,
    HID_ESCAPE = 0x29,
    HID_BACKSPACE = 0x2a,
    HID_TAB = 0x2b,
    HID_SPACE = 0x2c,
    HID_CAPS_LOCK = 0x39,
    HID_F1 = 0x3a,
    HID_F12 = 0x45,
    HID_INSERT = 0x49,
    HID_HOME = 0x4a,
    HID_PAGE_UP = 0x4b,
    HID_DELETE = 0x4c,
    HID_END = 0x4d,
    HID_PAGE_DOWN = 0x4e,
    HID_RIGHT = 0x4f,
    HID_LEFT = 0x50,
    HID_DOWN = 0x51,
    HID_UP = 0x52,
    HID_KEYPAD_ENTER = 0x58,
    HID_LEFT_CTRL = 0xe0,
    HID_LEFT_SHIFT = 0xe1,
    HID_LEFT_ALT = 0xe2,
    HID_LEFT_SUPER = 0xe3,
    HID_RIGHT_CTRL = 0xe4,
    HID_RIGHT_SHIFT = 0xe5,
    HID_RIGHT_ALT = 0xe6,
    HID_RIGHT_SUPER = 0xe7,
};

// The key each held HID usage pressed, since stlxdm sends a release without its character
static QHash<uint16_t, int> s_held_keys;

static bool is_printable(uint32_t ch) {
    return ch >= FIRST_PRINTABLE && ch <= LAST_PRINTABLE;
}

static int to_qt_key(uint16_t usage, uint32_t ch) {
    if (is_printable(ch)) {
        return (ch >= 'a' && ch <= 'z') ? Qt::Key_A + static_cast<int>(ch - 'a') : static_cast<int>(ch);
    }

    if (usage >= HID_F1 && usage <= HID_F12) {
        return Qt::Key_F1 + (usage - HID_F1);
    }

    switch (usage) {
        case HID_ENTER:
            return Qt::Key_Return;
        case HID_KEYPAD_ENTER:
            return Qt::Key_Enter;
        case HID_ESCAPE:
            return Qt::Key_Escape;
        case HID_BACKSPACE:
            return Qt::Key_Backspace;
        case HID_TAB:
            return Qt::Key_Tab;
        case HID_SPACE:
            return Qt::Key_Space;
        case HID_CAPS_LOCK:
            return Qt::Key_CapsLock;
        case HID_INSERT:
            return Qt::Key_Insert;
        case HID_HOME:
            return Qt::Key_Home;
        case HID_PAGE_UP:
            return Qt::Key_PageUp;
        case HID_DELETE:
            return Qt::Key_Delete;
        case HID_END:
            return Qt::Key_End;
        case HID_PAGE_DOWN:
            return Qt::Key_PageDown;
        case HID_RIGHT:
            return Qt::Key_Right;
        case HID_LEFT:
            return Qt::Key_Left;
        case HID_DOWN:
            return Qt::Key_Down;
        case HID_UP:
            return Qt::Key_Up;
        case HID_LEFT_CTRL:
        case HID_RIGHT_CTRL:
            return Qt::Key_Control;
        case HID_LEFT_SHIFT:
        case HID_RIGHT_SHIFT:
            return Qt::Key_Shift;
        case HID_LEFT_ALT:
        case HID_RIGHT_ALT:
            return Qt::Key_Alt;
        case HID_LEFT_SUPER:
        case HID_RIGHT_SUPER:
            return Qt::Key_Meta;
        default:
            return Qt::Key_unknown;
    }
}

static QString key_text(int key, uint32_t ch) {
    if (is_printable(ch)) {
        char32_t codepoint = ch;
        return QString::fromUcs4(&codepoint, 1);
    }

    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        return QStringLiteral("\r");
    }

    if (key == Qt::Key_Tab) {
        return QStringLiteral("\t");
    }

    return QString();
}

static Qt::KeyboardModifiers to_qt_modifiers(uint8_t modifiers) {
    Qt::KeyboardModifiers result = Qt::NoModifier;
    if (modifiers & STLXWIN_MOD_SHIFT) {
        result |= Qt::ShiftModifier;
    }

    if (modifiers & STLXWIN_MOD_CTRL) {
        result |= Qt::ControlModifier;
    }

    if (modifiers & STLXWIN_MOD_ALT) {
        result |= Qt::AltModifier;
    }

    if (modifiers & STLXWIN_MOD_SUPER) {
        result |= Qt::MetaModifier;
    }

    return result;
}

// stlxdm reads an unbounded side as 0
static uint32_t to_stlxwin_extent(int extent) {
    return extent >= QWINDOWSIZE_MAX ? 0 : static_cast<uint32_t>(qMax(extent, 0));
}

static Qt::MouseButton to_qt_button(uint8_t button) {
    switch (button) {
        case STLXWIN_BTN_LEFT:
            return Qt::LeftButton;
        case STLXWIN_BTN_RIGHT:
            return Qt::RightButton;
        case STLXWIN_BTN_MIDDLE:
            return Qt::MiddleButton;
        default:
            return Qt::NoButton;
    }
}

QStelluxWindow::QStelluxWindow(QWindow* window)
    : QPlatformWindow(window) {
    QRect requested = window->geometry();
    if (requested.size().isEmpty()) {
        requested.setSize(DEFAULT_WINDOW_SIZE);
    }

    QPlatformWindow::setGeometry(requested);
}

QStelluxWindow::~QStelluxWindow() {
    destroy_native_window();
}

stlxwin_window* QStelluxWindow::handle() const {
    return m_handle;
}

// A popup opened without a parent window, such as a context menu, opens over the focused one
QWindow* QStelluxWindow::popup_parent() const {
    Qt::WindowType type = window()->type();
    if (type != Qt::Popup && type != Qt::ToolTip) {
        return nullptr;
    }

    QWindow* parent = window()->transientParent();
    if (!parent) {
        parent = QGuiApplication::focusWindow();
    }

    if (!parent || parent == window() || !parent->handle()) {
        return nullptr;
    }

    return static_cast<QStelluxWindow*>(parent->handle())->m_handle ? parent : nullptr;
}

bool QStelluxWindow::is_popup() const {
    return !m_parent.isNull();
}

void QStelluxWindow::create_native_window() {
    QSize size = geometry().size();
    QStelluxIntegration* integration = QStelluxIntegration::instance();

    m_parent = popup_parent();
    if (is_popup()) {
        auto* parent_window = static_cast<QStelluxWindow*>(m_parent->handle());
        QPoint offset = geometry().topLeft() - m_parent->geometry().topLeft();
        uint32_t flags = window()->type() == Qt::Popup ? STLXWIN_PF_GRAB : 0;

        m_handle = stlxwin_popup_create(parent_window->handle(), offset.x(), offset.y(),
                                        static_cast<uint32_t>(size.width()),
                                        static_cast<uint32_t>(size.height()), flags);
    } else {
        QByteArray title = window()->title().toUtf8();
        m_handle = stlxwin_window_create(integration->connection(), static_cast<uint32_t>(size.width()),
                                         static_cast<uint32_t>(size.height()), title.constData(),
                                         STLXWIN_WF_RESIZABLE);
    }

    if (m_handle) {
        integration->add_window(m_handle, this);
        propagateSizeHints();
    }
}

void QStelluxWindow::destroy_native_window() {
    if (!m_handle) {
        return;
    }

    drop_child_popups();
    QStelluxIntegration::instance()->remove_window(m_handle);
    stlxwin_window_destroy(m_handle);
    m_handle = nullptr;
}

// stlxwin frees a window's popups along with it, so they only let go of their handles and close
void QStelluxWindow::drop_child_popups() {
    for (QStelluxWindow* popup : QStelluxIntegration::instance()->windows()) {
        if (popup->m_parent != window() || !popup->m_handle) {
            continue;
        }

        popup->drop_child_popups();
        QStelluxIntegration::instance()->remove_window(popup->m_handle);
        popup->m_handle = nullptr;
        QWindowSystemInterface::handleCloseEvent(popup->window());
    }
}

// stlxdm places a popup only as it creates one, so a shown popup that moves or resizes is made again
void QStelluxWindow::setGeometry(const QRect& rect) {
    bool changed = rect != geometry();
    QPlatformWindow::setGeometry(rect);
    if (!changed || !is_popup() || !m_handle) {
        return;
    }

    QWindowSystemInterface::handleGeometryChange(window(), rect);
    destroy_native_window();
    create_native_window();
    QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(), rect.size()));
}

void QStelluxWindow::setVisible(bool visible) {
    QPlatformWindow::setVisible(visible);
    m_visible = visible;

    // stlxdm has no hidden state, so a hidden window goes away and comes back new
    if (!visible) {
        destroy_native_window();
    } else if (!m_handle) {
        create_native_window();
    }

    QRegion exposed = visible ? QRegion(QRect(QPoint(), geometry().size())) : QRegion();
    QWindowSystemInterface::handleExposeEvent(window(), exposed);

    if (visible && window()->type() != Qt::ToolTip) {
        requestActivateWindow();
    }
}

void QStelluxWindow::setWindowTitle(const QString& title) {
    if (m_handle) {
        stlxwin_window_set_title(m_handle, title.toUtf8().constData());
    }
}

void QStelluxWindow::propagateSizeHints() {
    if (!m_handle || is_popup()) {
        return;
    }

    QSize minimum = windowMinimumSize();
    QSize maximum = windowMaximumSize();
    stlxwin_window_set_min_size(m_handle, to_stlxwin_extent(minimum.width()), to_stlxwin_extent(minimum.height()));
    stlxwin_window_set_max_size(m_handle, to_stlxwin_extent(maximum.width()), to_stlxwin_extent(maximum.height()));
}

bool QStelluxWindow::isExposed() const {
    return m_visible && m_handle;
}

void QStelluxWindow::present(const QImage& image, const QRegion& region) {
    if (!m_handle) {
        return;
    }

    stlxwin_buffer* buffer = stlxwin_begin_frame(m_handle);
    QStelluxIntegration::instance()->schedule_queued_events();
    if (!buffer) {
        return;
    }

    // A buffer can hold an older frame, so every commit carries the whole image
    int width = qMin(static_cast<int>(buffer->width), image.width());
    int height = qMin(static_cast<int>(buffer->height), image.height());
    auto* pixels = reinterpret_cast<uint8_t*>(buffer->pixels);
    for (int y = 0; y < height; y++) {
        std::memcpy(pixels + static_cast<size_t>(y) * buffer->stride, image.constScanLine(y),
                    static_cast<size_t>(width) * BYTES_PER_PIXEL);
    }

    QVarLengthArray<stlxwin_rect, 16> damage;
    for (const QRect& rect : region) {
        damage.append({rect.x(), rect.y(), rect.width(), rect.height()});
    }

    stlxwin_commit(m_handle, buffer, damage.constData(), static_cast<uint32_t>(damage.size()), 0);
}

QPointF QStelluxWindow::global_position(const QPointF& local) const {
    return local + geometry().topLeft();
}

void QStelluxWindow::handle_event(const stlxwin_event& event) {
    switch (event.type) {
        case STLXWIN_EVT_KEY_DOWN:
        case STLXWIN_EVT_KEY_UP:
        case STLXWIN_EVT_KEY_REPEAT:
            handle_key(event);
            break;
        case STLXWIN_EVT_POINTER_MOTION:
        case STLXWIN_EVT_BUTTON_DOWN:
        case STLXWIN_EVT_BUTTON_UP:
        case STLXWIN_EVT_SCROLL:
        case STLXWIN_EVT_POINTER_ENTER:
        case STLXWIN_EVT_POINTER_LEAVE:
            handle_pointer(event);
            break;
        case STLXWIN_EVT_FOCUS_IN:
            QWindowSystemInterface::handleFocusWindowChanged(window(), Qt::ActiveWindowFocusReason);
            break;
        case STLXWIN_EVT_FOCUS_OUT:
            if (QGuiApplication::focusWindow() == window()) {
                QWindowSystemInterface::handleFocusWindowChanged(nullptr, Qt::ActiveWindowFocusReason);
            }

            break;
        case STLXWIN_EVT_CLOSE:
            QWindowSystemInterface::handleCloseEvent(window());
            break;
        case STLXWIN_EVT_CONFIGURE:
            handle_configure(event);
            break;
        case STLXWIN_EVT_POPUP_DISMISSED:
            close_dismissed_popups();
            break;
        default:
            break;
    }
}

void QStelluxWindow::handle_key(const stlxwin_event& event) {
    m_modifiers = to_qt_modifiers(event.key.modifiers);

    uint16_t usage = event.key.usage;
    int key = to_qt_key(usage, event.key.ch);
    if (event.type == STLXWIN_EVT_KEY_UP) {
        key = s_held_keys.value(usage, key);
        s_held_keys.remove(usage);
    } else {
        s_held_keys.insert(usage, key);
    }

    QEvent::Type type = event.type == STLXWIN_EVT_KEY_UP ? QEvent::KeyRelease : QEvent::KeyPress;
    bool is_repeat = event.type == STLXWIN_EVT_KEY_REPEAT;

    QWindowSystemInterface::handleKeyEvent(window(), type, key, m_modifiers, key_text(key, event.key.ch),
                                           is_repeat);
}

void QStelluxWindow::handle_pointer(const stlxwin_event& event) {
    switch (event.type) {
        case STLXWIN_EVT_POINTER_MOTION: {
            QPointF local(event.motion.x, event.motion.y);
            QWindowSystemInterface::handleMouseEvent(window(), local, global_position(local), m_buttons,
                                                     Qt::NoButton, QEvent::MouseMove, m_modifiers);
            break;
        }
        case STLXWIN_EVT_BUTTON_DOWN:
        case STLXWIN_EVT_BUTTON_UP: {
            QPointF local(event.button.x, event.button.y);
            Qt::MouseButton button = to_qt_button(event.button.button);
            bool is_press = event.type == STLXWIN_EVT_BUTTON_DOWN;

            m_buttons = is_press ? (m_buttons | button) : (m_buttons & ~button);
            QWindowSystemInterface::handleMouseEvent(window(), local, global_position(local), m_buttons, button,
                                                     is_press ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
                                                     m_modifiers);
            break;
        }
        case STLXWIN_EVT_SCROLL: {
            // A positive detent scrolls toward the top, as a positive angle delta does
            QPointF local(event.scroll.x, event.scroll.y);
            QPoint angle(0, event.scroll.dy * QWheelEvent::DefaultDeltasPerStep);
            QWindowSystemInterface::handleWheelEvent(window(), local, global_position(local), QPoint(), angle,
                                                     m_modifiers);
            break;
        }
        case STLXWIN_EVT_POINTER_ENTER: {
            QPointF local(event.motion.x, event.motion.y);
            QWindowSystemInterface::handleEnterEvent(window(), local, global_position(local));
            break;
        }
        case STLXWIN_EVT_POINTER_LEAVE:
            QWindowSystemInterface::handleLeaveEvent(window());
            break;
        default:
            break;
    }
}

void QStelluxWindow::handle_configure(const stlxwin_event& event) {
    QSize size(static_cast<int>(event.configure.width), static_cast<int>(event.configure.height));
    if (size.isEmpty()) {
        return;
    }

    // Every configure must be answered with a commit, which this expose triggers
    QRect configured(geometry().topLeft(), size);
    QPlatformWindow::setGeometry(configured);
    QWindowSystemInterface::handleGeometryChange(window(), configured);
    QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(), size));
}

// stlxdm has destroyed a dismissed popup already, so its client side is freed
// before Qt could commit another frame to it
void QStelluxWindow::close_dismissed_popups() {
    for (QStelluxWindow* candidate : QStelluxIntegration::instance()->windows()) {
        QWindow* popup = candidate->window();
        if (popup->type() == Qt::Popup && candidate->m_parent == window()) {
            candidate->destroy_native_window();
            QWindowSystemInterface::handleCloseEvent(popup);
        }
    }
}
