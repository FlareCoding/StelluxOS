#ifndef SHEEN_PICKER_H
#define SHEEN_PICKER_H

/* The one picker for everything Sheen filters: a panel at the top center over the text with a query field, its prefix
 * and a help line. Go to line is its first use, with no list under the field. */
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;

class picker : public QWidget {
public:
    explicit picker(QWidget* parent);

    /* Shows the panel over its parent with `prefix` before an empty query, `help` under it and `aside` at the help
     * line's right. Both are rich text, so emphasized() and code() can set parts of them off */
    void open(const QString& prefix, const QString& help, const QString& aside);

    void set_help(const QString& help);

    /* `text` set off in a help line, as its numbers are */
    static QString emphasized(const QString& text);

    /* `text` in a help line's code face, as a position such as Ln 42 is */
    static QString code(const QString& text);

    std::function<void(const QString& query)> on_changed;
    std::function<void(const QString& query)> on_accepted;
    std::function<void()> on_cancelled;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void finish(bool is_accepted);

    QLabel* m_prefix;
    QLineEdit* m_query;
    QLabel* m_help;
    QLabel* m_aside;
};

#endif
