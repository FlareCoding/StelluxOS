#ifndef STLXQPA_WINDOW_H
#define STLXQPA_WINDOW_H

#include <QImage>
#include <QList>
#include <QPointer>
#include <QRegion>
#include <qpa/qplatformwindow.h>

#include <memory>

#include <stlxwin/stlxwin.h>

class QMimeData;
class QStelluxMimeData;

/**
 * A Qt window shown by stlxdm: a toplevel, or a popup placed relative to the
 * window that opened it. Frames arrive from the backing store through present.
 */
class QStelluxWindow : public QPlatformWindow {
public:
    explicit QStelluxWindow(QWindow* window);
    ~QStelluxWindow() override;

    stlxwin_window* handle() const;
    bool shows_glass() const;
    void present(const QImage& image, const QRegion& region);
    void handle_event(const stlxwin_event& event);

    // Lets go of every button, whose release a drag took
    void forget_buttons();

    // Words for the chip of the drag over the window, which the next answer to it carries
    void set_drop_words(const QString& words);

    void setGeometry(const QRect& rect) override;
    void setVisible(bool visible) override;
    void setWindowTitle(const QString& title) override;
    void propagateSizeHints() override;
    void requestActivateWindow() override;
    bool isExposed() const override;

private:
    QWindow* popup_parent() const;
    bool is_popup() const;
    void create_native_window();
    void destroy_native_window();
    void drop_child_popups();
    void close_dismissed_popups();
    void handle_key(const stlxwin_event& event);
    void handle_pointer(const stlxwin_event& event);
    void paste();
    void handle_configure(const stlxwin_event& event);
    void handle_window_output();
    void handle_drop_motion(const stlxwin_event& event);
    void handle_drop_leave();
    void handle_drop(const stlxwin_event& event);
    const QMimeData* drop_data() const;
    QPointF global_position(const QPointF& local) const;

    stlxwin_window* m_handle = nullptr;

    // Decided once, so every native window and backing store image of the window agree on the pixel format
    bool m_shows_glass;

    // The window a popup opened over, kept while it is shown since focus moves into the popup
    QPointer<QWindow> m_parent;

    // The damage of the last frames committed, newest first, which a buffer holding an older frame still lacks
    QList<QRegion> m_recent_damage;

    Qt::MouseButtons m_buttons = Qt::NoButton;
    Qt::KeyboardModifiers m_modifiers = Qt::NoModifier;
    bool m_visible = false;

    // The drag over the window: the desktop's item, read once dropped, the actions its source allows, whether it
    // comes from this app, whose own QMimeData Qt then reads, and the words the widget under the pointer set
    std::unique_ptr<QStelluxMimeData> m_drop_data;
    Qt::DropActions m_drop_actions = Qt::IgnoreAction;
    bool m_drop_is_own = false;
    QString m_drop_words;
};

#endif
