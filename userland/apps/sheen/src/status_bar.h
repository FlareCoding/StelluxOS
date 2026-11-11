#ifndef SHEEN_STATUS_BAR_H
#define SHEEN_STATUS_BAR_H

/* The line under the text on the window's glass: the caret's place, the selection and the indentation, a message,
 * and the file's facts at the right, each opening its menu through `on_fact_clicked`. */
#include <QStringList>
#include <QWidget>

#include <functional>

enum class file_fact { LANGUAGE, ENCODING, LINE_ENDING, COUNT };

class status_bar : public QWidget {
public:
    explicit status_bar(QWidget* parent);

    void set_caret_parts(const QStringList& parts);
    void set_message(const QString& message);
    void set_facts(const QStringList& facts);

    std::function<void(file_fact fact, const QPoint& anchor)> on_fact_clicked;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRect fact_rect(int index) const;
    int fact_at(const QPoint& point) const;

    QFont m_font;
    QStringList m_caret_parts;
    QString m_message;
    QStringList m_facts;
    int m_hovered_fact = -1;
};

#endif
