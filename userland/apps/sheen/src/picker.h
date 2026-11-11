#ifndef SHEEN_PICKER_H
#define SHEEN_PICKER_H

/* The one picker for everything Sheen filters: a panel at the top center over the text with a query field, its prefix
 * and either a help line or a list. Go to line uses the help line, the encoding pickers the list. */
#include <QList>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;

/* A row of a picker's list. `section` heads the run of rows sharing it while the query is empty */
struct picker_row {
    QString label;
    QString section;
    bool is_current = false;
};

class picker : public QWidget {
public:
    explicit picker(QWidget* parent);

    /* Shows the panel over its parent with `prefix` before an empty query, `help` under it and `aside` at the help
     * line's right. Both are rich text, so emphasized() and code() can set parts of them off */
    void open(const QString& prefix, const QString& help, const QString& aside);

    /* Shows the panel over its parent with `rows` under an empty query, which filters them as typed and reads
     * `placeholder` while empty. Enter or a click chooses the lit row */
    void open_list(const QString& placeholder, const QList<picker_row>& rows);

    void set_help(const QString& help);

    /* `text` set off in a help line, as its numbers are */
    static QString emphasized(const QString& text);

    /* `text` in a help line's code face, as a position such as Ln 42 is */
    static QString code(const QString& text);

    std::function<void(const QString& query)> on_changed;
    std::function<void(const QString& query)> on_accepted;
    std::function<void(int row)> on_chosen;
    std::function<void()> on_cancelled;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void present();
    void filter_rows(const QString& query);
    void step(int delta);
    void finish(bool is_accepted);

    QLabel* m_prefix;
    QLineEdit* m_query;
    QWidget* m_help_row;
    QLabel* m_help;
    QLabel* m_aside;
    QListWidget* m_list;
    QList<picker_row> m_rows;
    QString m_filter;
};

#endif
