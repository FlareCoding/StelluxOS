#ifndef SHEEN_TAB_STRIP_H
#define SHEEN_TAB_STRIP_H

/* The row of open files above the text, painted from the theme's tokens on the glass. An unsaved file shows an
 * amber dot in its close slot, which turns into the close glyph under the pointer. */
#include <QTabBar>
#include <QWidget>

#include <functional>

class menu_button;

class tab_strip : public QTabBar {
public:
    explicit tab_strip(QWidget* parent);

    void set_tab_modified(int index, bool is_modified);

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
    void set_hover(int tab, bool is_slot_hovered);

    QFont m_font;
    int m_hovered_tab = -1;
    bool m_is_slot_hovered = false;
    int m_pressed_slot = -1;
};

/* The menu button, the strip and the hairline under them, left open beneath the open tab */
class tab_row : public QWidget {
public:
    explicit tab_row(QWidget* parent);

    tab_strip* tabs() const { return m_tabs; }
    menu_button* menu() const { return m_menu; }

    /* A double click on the strip past the last tab */
    std::function<void()> on_empty_double_click;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    menu_button* m_menu;
    tab_strip* m_tabs;
};

#endif
