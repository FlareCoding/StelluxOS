#ifndef SHEEN_KEYCAPS_H
#define SHEEN_KEYCAPS_H

/* Keys drawn as keycaps in the look of the system menu's shortcuts, such as Ctrl and L */
#include <QStringList>
#include <QWidget>

class QColor;
class QPainter;

/* The keys of a shortcut's text, such as Ctrl+Shift+S, each named as the keyboard map names it */
QStringList shortcut_keys(const QString& shortcut);

/* The width of `keys` drawn side by side as keycaps */
int keycaps_width(const QStringList& keys);

/* Paints `keys` as keycaps from `left`, centered on `center_y`, their names in `color` */
void paint_keycaps(QPainter* painter, const QStringList& keys, int left, int center_y, const QColor& color);

/* A shortcut's keycaps followed by what it does, such as Ctrl L type a path */
class keycap_hint : public QWidget {
public:
    keycap_hint(const QStringList& keys, const QString& text, QWidget* parent);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QStringList m_keys;
    QString m_text;
};

#endif
