#ifndef STLXQPA_WINDOW_H
#define STLXQPA_WINDOW_H

#include <QImage>
#include <QPointer>
#include <qpa/qplatformwindow.h>

#include <stlxwin/stlxwin.h>

/**
 * A Qt window shown by stlxdm: a toplevel, or a popup placed relative to the
 * window that opened it. Frames arrive from the backing store through present.
 */
class QStelluxWindow : public QPlatformWindow {
public:
    explicit QStelluxWindow(QWindow* window);
    ~QStelluxWindow() override;

    stlxwin_window* handle() const;
    void present(const QImage& image, const QRegion& region);
    void handle_event(const stlxwin_event& event);

    void setGeometry(const QRect& rect) override;
    void setVisible(bool visible) override;
    void setWindowTitle(const QString& title) override;
    void propagateSizeHints() override;
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
    void handle_configure(const stlxwin_event& event);
    QPointF global_position(const QPointF& local) const;

    stlxwin_window* m_handle = nullptr;

    // The window a popup opened over, kept while it is shown since focus moves into the popup
    QPointer<QWindow> m_parent;

    Qt::MouseButtons m_buttons = Qt::NoButton;
    Qt::KeyboardModifiers m_modifiers = Qt::NoModifier;
    bool m_visible = false;
};

#endif
