#ifndef SHEEN_MENU_STYLE_H
#define SHEEN_MENU_STYLE_H

/* Sheen's menus in the look of the system menu: keycaps, a check column, chevrons and inset hairlines. Only
 * Sheen's own menus take it, so other programs keep the toolkit's menus. */
#include <QAbstractButton>
#include <QProxyStyle>

class menu_style : public QProxyStyle {
public:
    menu_style();

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contents,
                           const QWidget* widget) const override;
};

/* The button at the left end of the tab strip that opens the menus, a three line glyph on the glass */
class menu_button : public QAbstractButton {
public:
    explicit menu_button(QWidget* parent);

    void set_open(bool is_open);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    bool m_is_open = false;
};

#endif
