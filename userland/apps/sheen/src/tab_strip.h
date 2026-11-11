#ifndef SHEEN_TAB_STRIP_H
#define SHEEN_TAB_STRIP_H

/* The row of open files above the text, painted from the theme's tokens on the glass. An unsaved file shows an
 * amber dot in its close slot, which turns into the close glyph under the pointer, and a read-only one a lock. */
#include <QAbstractButton>
#include <QTabBar>
#include <QWidget>

#include <functional>

class menu_button;

class tab_strip : public QTabBar {
public:
    explicit tab_strip(QWidget* parent);

    void set_tab_modified(int index, bool is_modified);

    /* Set before the tab's name, whose change lays the tabs out again for the lock's width */
    void set_tab_locked(int index, bool is_locked);

    /* Strikes the tab's name through, as for a file deleted on disk */
    void set_tab_struck(int index, bool is_struck);

    /* `name`, then `hint` in the dimmest tone, such as the folder that tells two same-named files apart */
    void set_tab_name(int index, const QString& name, const QString& hint);

    /* How much of a loading file has arrived, in thousandths, drawn along the tab's bottom edge. -1 draws none */
    void set_tab_progress(int index, int permille);

    /* The part of the strip the row shows, whose clipped ends fade */
    void set_visible_span(int offset, int width);

    std::function<void(int index, const QPoint& anchor)> on_context_menu;

protected:
    QSize tabSizeHint(int index) const override;
    QSize minimumTabSizeHint(int index) const override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRect close_slot(int index) const;
    bool is_tab_modified(int index) const;
    bool is_tab_locked(int index) const;
    bool is_tab_struck(int index) const;
    int tab_progress(int index) const;
    void set_tab_flag(int index, int flag, bool is_set);
    void set_hover(int tab, bool is_slot_hovered);

    QFont m_font;
    int m_hovered_tab = -1;
    bool m_is_slot_hovered = false;
    int m_pressed_slot = -1;
    int m_span_offset = 0;
    int m_span_width = 0;
};

/* The count of open tabs with a chevron, at the right end of a strip too long for its row */
class tab_list_button : public QAbstractButton {
public:
    explicit tab_list_button(QWidget* parent);

    void set_open(bool is_open);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    bool m_is_open = false;
};

/* The menu button, the strip and the hairline under them, left open beneath the open tab. A strip longer than the
 * row scrolls, by the wheel and to keep the open tab in view, and the tab list button appears at its end. */
class tab_row : public QWidget {
public:
    explicit tab_row(QWidget* parent);

    tab_strip* tabs() const { return m_tabs; }
    menu_button* menu() const { return m_menu; }
    tab_list_button* tab_list() const { return m_tab_list; }

    /* A double click on the strip past the last tab */
    std::function<void()> on_empty_double_click;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void place_strip(bool shows_current);

    menu_button* m_menu;
    QWidget* m_viewport;
    tab_strip* m_tabs;
    tab_list_button* m_tab_list;
    int m_offset = 0;
};

#endif
