#ifndef SHEEN_STATUS_BAR_H
#define SHEEN_STATUS_BAR_H

/* The line under the text on the window's glass: the caret's place, the selection and the indentation at the left,
 * a note, and the file's mode, language, encoding and line endings at the right. Its buttons open their menus. */
#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

enum class status_kind { POSITION, SELECTION, INDENTATION, OVERTYPE, MODE, LANGUAGE, ENCODING, LINE_ENDING };

struct status_item {
    status_kind kind;
    QString text;
    QString tooltip;
    bool is_button;
};

class status_bar : public QWidget {
public:
    explicit status_bar(QWidget* parent);

    void set_items(const QList<status_item>& left, const QList<status_item>& right);
    void set_message(const QString& message);

    /* Draws the item of `kind` as open while its menu shows, null for none */
    void set_open_item(const status_kind* kind);

    std::function<void(status_kind kind, const QPoint& anchor)> on_item_clicked;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct placed_item {
        status_item item;
        QRect rect;
        bool is_right;
    };

    void place_items();
    int item_at(const QPoint& point) const;

    QFont m_font;
    QList<status_item> m_left;
    QList<status_item> m_right;
    QList<placed_item> m_placed;
    QString m_message;
    int m_hovered = -1;
    bool m_has_open = false;
    status_kind m_open_kind = status_kind::POSITION;
};

#endif
