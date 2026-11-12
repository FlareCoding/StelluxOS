#include "window.h"
#include "clipboard.h"
#include "drag.h"
#include "integration.h"

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QDrag>
#include <QGuiApplication>
#include <QHash>
#include <QInputMethodQueryEvent>
#include <QKeySequence>
#include <QPalette>
#include <QVarLengthArray>
#include <QWheelEvent>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <stlxqpa/drop.h>

#include <algorithm>
#include <cstring>

static const QSize DEFAULT_WINDOW_SIZE(640, 480);
static const int BYTES_PER_PIXEL = 4;

// Frames whose damage present remembers, enough for a buffer held a few frames before it comes back
static const qsizetype DAMAGE_HISTORY = 4;
static const int FIRST_PRINTABLE = 0x20;
static const int LAST_PRINTABLE = 0x7e;
static const int OPAQUE_ALPHA = 255;

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

namespace {

// Remembers the object a drop reaches past its window, the widget under the pointer in a widget app
class drop_receiver_filter : public QObject {
public:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Drop && !watched->isWindowType()) {
            receiver = watched;
        }

        return false;
    }

    QPointer<QObject> receiver;
};

} // namespace

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

// A translucent Window color lets stlxdm's glass show through every window Qt paints
static bool palette_shows_glass() {
    return QGuiApplication::palette().color(QPalette::Window).alpha() < OPAQUE_ALPHA;
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
    : QPlatformWindow(window), m_shows_glass(palette_shows_glass()) {
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

bool QStelluxWindow::shows_glass() const {
    return m_shows_glass;
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
        bool grabs = window()->type() == Qt::Popup;
        uint32_t flags = grabs ? STLXWIN_PF_GRAB : 0;
        if (m_shows_glass) {
            flags |= STLXWIN_PF_GLASS | STLXWIN_PF_PREMULTIPLIED;
        }

        // Grabbing popups get rounded glass to match the style's rounded menus and combo lists, tooltips stay square
        if (m_shows_glass && grabs) {
            flags |= STLXWIN_PF_ROUNDED;
        }

        m_handle = stlxwin_popup_create(parent_window->handle(), offset.x(), offset.y(),
                                        static_cast<uint32_t>(size.width()),
                                        static_cast<uint32_t>(size.height()), flags);
    } else {
        QByteArray title = window()->title().toUtf8();
        uint32_t flags = STLXWIN_WF_RESIZABLE;
        if (m_shows_glass) {
            flags |= STLXWIN_WF_GLASS | STLXWIN_WF_PREMULTIPLIED;
        }

        m_handle = stlxwin_window_create(
            integration->connection(),
            static_cast<uint32_t>(size.width()),
            static_cast<uint32_t>(size.height()),
            title.constData(),
            flags
        );
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

    // Qt takes a newly shown window as active at once, as stlxdm focuses new toplevels and grabbing popups itself
    if (visible && window()->type() != Qt::ToolTip) {
        QPlatformWindow::requestActivateWindow();
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

// stlxdm decides whether a shown window comes forward, and Qt learns it from the focus event that follows
void QStelluxWindow::requestActivateWindow() {
    if (m_handle) {
        stlxwin_window_activate(m_handle);
    }
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

    // The buffer still holds the frame of buffer->age commits ago, so only what changed since then is copied
    const QRect bounds = QRect(0, 0, static_cast<int>(buffer->width), static_cast<int>(buffer->height)) & image.rect();
    QRegion changed = region;
    if (buffer->age == 0 || buffer->age - 1 > static_cast<uint32_t>(m_recent_damage.size())) {
        changed = bounds;
    } else {
        for (uint32_t i = 0; i + 1 < buffer->age; i++) {
            changed += m_recent_damage.at(i);
        }
    }

    auto* pixels = reinterpret_cast<uint8_t*>(buffer->pixels);
    for (const QRect& rect : changed & bounds) {
        size_t column_offset = static_cast<size_t>(rect.left()) * BYTES_PER_PIXEL;
        size_t row_bytes = static_cast<size_t>(rect.width()) * BYTES_PER_PIXEL;
        for (int y = rect.top(); y <= rect.bottom(); y++) {
            uint8_t* row = pixels + static_cast<size_t>(y) * buffer->stride;
            std::memcpy(row + column_offset, image.constScanLine(y) + column_offset, row_bytes);
        }
    }

    m_recent_damage.prepend(region);
    if (m_recent_damage.size() > DAMAGE_HISTORY) {
        m_recent_damage.removeLast();
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
        case STLXWIN_EVT_WINDOW_OUTPUT:
            handle_window_output();
            break;
        case STLXWIN_EVT_PASTE:
            paste();
            break;
        case STLXWIN_EVT_DROP_ENTER:
        case STLXWIN_EVT_DROP_MOTION:
            handle_drop_motion(event);
            break;
        case STLXWIN_EVT_DROP_LEAVE:
            handle_drop_leave();
            break;
        case STLXWIN_EVT_DROP:
            handle_drop(event);
            break;
        default:
            break;
    }
}

void QStelluxWindow::forget_buttons() {
    m_buttons = Qt::NoButton;
}

void QStelluxWindow::set_drop_words(const QString& words) {
    m_drop_words = words;
}

// The desktop's paste reaches the focus object as the theme's Paste key sequence, through shortcuts first, so every
// widget pastes its own way. The release carries the held modifiers, which leaves Qt's modifier state as it was.
void QStelluxWindow::paste() {
    QList<QKeySequence> bindings = QKeySequence::keyBindings(QKeySequence::Paste);
    QKeyCombination combination = bindings.isEmpty() ? QKeyCombination(Qt::ControlModifier, Qt::Key_V) : bindings[0][0];

    QWindowSystemInterface::handleKeyEvent(window(), QEvent::KeyPress, combination.key(),
                                           combination.keyboardModifiers());
    QWindowSystemInterface::handleKeyEvent(window(), QEvent::KeyRelease, combination.key(), m_modifiers);
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
            m_modifiers = to_qt_modifiers(event.motion.modifiers);
            QStelluxIntegration::instance()->note_pointer(global_position(local).toPoint());
            QWindowSystemInterface::handleMouseEvent(window(), local, global_position(local), m_buttons,
                                                     Qt::NoButton, QEvent::MouseMove, m_modifiers);
            break;
        }
        case STLXWIN_EVT_BUTTON_DOWN:
        case STLXWIN_EVT_BUTTON_UP: {
            QPointF local(event.button.x, event.button.y);
            Qt::MouseButton button = to_qt_button(event.button.button);
            bool is_press = event.type == STLXWIN_EVT_BUTTON_DOWN;

            // A drag starting from this press names it by its serial
            if (is_press) {
                QStelluxIntegration::instance()->stellux_drag()->note_press(window(), event.button.serial);
            }

            m_buttons = is_press ? (m_buttons | button) : (m_buttons & ~button);
            m_modifiers = to_qt_modifiers(event.button.modifiers);
            QStelluxIntegration::instance()->note_pointer(global_position(local).toPoint());
            QWindowSystemInterface::handleMouseEvent(window(), local, global_position(local), m_buttons, button,
                                                     is_press ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
                                                     m_modifiers);
            break;
        }
        case STLXWIN_EVT_SCROLL: {
            // A positive detent scrolls toward the top, as a positive angle delta does
            QPointF local(event.scroll.x, event.scroll.y);
            QPoint angle(0, event.scroll.dy * QWheelEvent::DefaultDeltasPerStep);
            m_modifiers = to_qt_modifiers(event.scroll.modifiers);
            QWindowSystemInterface::handleWheelEvent(window(), local, global_position(local), QPoint(), angle,
                                                     m_modifiers);
            break;
        }
        case STLXWIN_EVT_POINTER_ENTER: {
            QPointF local(event.motion.x, event.motion.y);
            m_modifiers = to_qt_modifiers(event.motion.modifiers);
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

// stlxdm places toplevels itself, so a toplevel takes the screen and position stlxdm names
void QStelluxWindow::handle_window_output() {
    uint32_t output = 0;
    int32_t x = 0;
    int32_t y = 0;
    if (stlxwin_window_get_output(m_handle, &output, &x, &y) != 0) {
        return;
    }

    QStelluxScreen* screen = QStelluxIntegration::instance()->screen_of(output);
    if (screen) {
        QWindowSystemInterface::handleWindowScreenChanged(window(), screen->screen());
    }

    QRect placed(QPoint(x, y), geometry().size());
    QPlatformWindow::setGeometry(placed);
    QWindowSystemInterface::handleGeometryChange(window(), placed);
}

// Whether the object takes typed text, as a field a drop of text should focus does
static bool takes_text_input(QObject* object) {
    if (!object) {
        return false;
    }

    QInputMethodQueryEvent query(Qt::ImEnabled);
    QCoreApplication::sendEvent(object, &query);

    return query.value(Qt::ImEnabled).toBool();
}

// The widget a drop typed into: the one it reached, or the scrolling view whose viewport it reached, as QTextEdit and
// Scintilla take their drops
static QObject* text_input_of(QObject* receiver) {
    auto* area = receiver ? qobject_cast<QAbstractScrollArea*>(receiver->parent()) : nullptr;
    QObject* widget = area && area->viewport() == receiver ? area : receiver;

    return takes_text_input(widget) ? widget : nullptr;
}

// The words as the UTF-8 a DROP_STATUS holds, cut before a character that would not fit whole
static QByteArray fitted_words(const QString& words) {
    QByteArray utf8 = words.toUtf8();
    qsizetype end = std::min<qsizetype>(utf8.size(), STLXWIN_DROP_WORDS_MAX - 1);
    while (end > 0 && end < utf8.size() && (static_cast<uchar>(utf8[end]) & 0xC0) == 0x80) {
        end--;
    }

    return utf8.left(end);
}

// A drag of this app's reads as Qt's own QMimeData, so formats that never leave the app still drop
const QMimeData* QStelluxWindow::drop_data() const {
    QDrag* own = m_drop_is_own ? QStelluxIntegration::instance()->drag()->currentDrag() : nullptr;
    return own ? own->mimeData() : m_drop_data.get();
}

// Each move over the window asks Qt where a drop would land and tells stlxdm the action the widget there takes, with
// the words it set for that action
void QStelluxWindow::handle_drop_motion(const stlxwin_event& event) {
    if (event.type == STLXWIN_EVT_DROP_ENTER) {
        stlxwin_offer_types types = {};
        stlxwin_drop_types(m_handle, &types);

        stlxwin_window* handle = m_handle;
        m_drop_data = std::make_unique<QStelluxMimeData>([handle](const char* mime_type, char** out_data) {
            return stlxwin_drop_read(handle, mime_type, out_data);
        });
        m_drop_data->show_types(types);
        m_drop_actions = to_qt_actions(event.drop.actions);
        m_drop_is_own = (event.drop.flags & STLXWIN_DROP_OWN) != 0;
    }

    QPoint local(event.drop.x, event.drop.y);
    m_modifiers = to_qt_modifiers(event.drop.modifiers);
    QStelluxIntegration::instance()->note_pointer(global_position(local).toPoint());

    m_drop_words.clear();
    QPlatformDragQtResponse answer = QWindowSystemInterface::handleDrag(window(), drop_data(), local, m_drop_actions,
                                                                      Qt::LeftButton, m_modifiers);
    uint32_t action = answer.isAccepted() ? to_stlxwin_actions(answer.acceptedAction()) : 0;
    QByteArray words = fitted_words(m_drop_words);
    bool has_words = action != 0 && !words.isEmpty();
    stlxwin_drop_status(m_handle, action, action, has_words ? action : 0, has_words ? words.constData() : nullptr);
}

void QStelluxWindow::handle_drop_leave() {
    QWindowSystemInterface::handleDrag(window(), nullptr, QPoint(), Qt::IgnoreAction, Qt::NoButton, m_modifiers);
    m_drop_data.reset();
}

// Qt drops with only the action stlxdm chose, so the widget takes that one. A drop into a field that takes text gives
// the field the window's focus and the window the desktop's, as Enter should then reach it.
void QStelluxWindow::handle_drop(const stlxwin_event& event) {
    QPoint local(event.drop.x, event.drop.y);
    m_modifiers = to_qt_modifiers(event.drop.modifiers);

    drop_receiver_filter filter;
    QCoreApplication::instance()->installEventFilter(&filter);
    QPlatformDropQtResponse answer = QWindowSystemInterface::handleDrop(window(), drop_data(), local,
                                                                      to_qt_actions(event.drop.action), Qt::NoButton,
                                                                      m_modifiers);
    QCoreApplication::instance()->removeEventFilter(&filter);

    uint32_t action = answer.isAccepted() ? to_stlxwin_actions(answer.acceptedAction()) : 0;
    QObject* typed_into = text_input_of(filter.receiver);
    bool focuses = action != 0 && typed_into;
    if (focuses) {
        QMetaObject::invokeMethod(typed_into, "setFocus");
    }

    stlxwin_drop_finish(m_handle, action, focuses ? STLXWIN_DROP_FOCUS : 0);
    m_drop_data.reset();
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

namespace stlxqpa {

// Every window of this platform is a QStelluxWindow, so a window's handle holds the words for its drag
void set_drop_words(QWindow* window, const QString& words) {
    auto* shown = window ? static_cast<QStelluxWindow*>(window->handle()) : nullptr;
    if (shown) {
        shown->set_drop_words(words);
    }
}

} // namespace stlxqpa
