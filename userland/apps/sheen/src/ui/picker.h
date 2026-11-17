#ifndef SHEEN_PICKER_H
#define SHEEN_PICKER_H

/* The one picker for everything Sheen filters: a panel at the top center over the text with a query field, its prefix
 * and either a help line or a list. Go to line uses the help line, the encoding pickers the list. */
#include <QList>
#include <QStringList>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;

/* A row of a picker's list. `section` heads the run of rows sharing it while the query is empty, `detail` leads the
 * label in a dim tone, `keys` or else `note` sit at the right, and `marks` are the label's letters the query matched */
struct picker_row {
    QString label;
    QString section;
    bool is_current = false;
    QString detail;
    QStringList keys;
    QString note;
    QList<int> marks;
};

class picker : public QWidget {
public:
    explicit picker(QWidget* parent);

    /* Shows the panel over its parent with `prefix` before an empty query, `help` under it and `aside` at the help
     * line's right. Both are rich text, so emphasized() and code() can set parts of them off */
    void open(const QString& prefix, const QString& help, const QString& aside);

    /* Shows `rows` under `query`, filtered as typed unless on_changed answers through set_rows, with `help` and
     * `aside` under the list when given. Enter or a click chooses the lit row */
    void open_list(const QString& placeholder, const QList<picker_row>& rows, const QString& query = QString(),
                   const QString& help = QString(), const QString& aside = QString());

    /* Replaces the list with `rows`, already filtered and marked for the query, the first one lit */
    void set_rows(const QList<picker_row>& rows);

    void set_help(const QString& help);

    /* `text` set off in a help line, as its numbers are */
    static QString emphasized(const QString& text);

    /* `text` in a help line's code face, as a position such as Ln 42 is */
    static QString code(const QString& text);

    std::function<void(const QString& query)> on_changed;
    std::function<void(const QString& query)> on_accepted;
    std::function<void(int row)> on_chosen;
    std::function<void()> on_cancelled;

    /* Backspace with the query empty, as if erasing the prefix, which only a mode the palette switched to sets */
    std::function<void()> on_prefix_erased;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void present(const QString& query);
    void filter_rows(const QString& query);
    void show_rows(const QList<int>& indices, bool has_headings);
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
