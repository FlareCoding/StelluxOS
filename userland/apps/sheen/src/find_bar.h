#ifndef SHEEN_FIND_BAR_H
#define SHEEN_FIND_BAR_H

/* The find bar, docked under the tab strip on glass: a query with its toggles, the match count, steps and close, and
 * the replace row Ctrl H adds. It keeps its query for the window and searches whichever view it is given. */
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <Scintilla.h>

#include <functional>

class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;
class ScintillaEditBase;
class field_frame;

class find_bar : public QWidget {
public:
    explicit find_bar(QWidget* parent);

    /* The view searches run in, null while the tab shows no text */
    void set_view(ScintillaEditBase* view);

    /* Opens the bar seeded from a one-line selection or the word at the caret, with the replace row when asked. A
     * selection of several lines becomes the range searched instead */
    void open(bool with_replace);

    /* Closes the bar, leaving the match selected for the text to take the focus */
    void close_bar();

    /* Steps to the next or previous match from the selection, wrapping at either end. False without a query */
    bool step(bool is_forward);

    /* Searches for the word at the caret, or the selection, without opening the bar */
    void find_word_at_caret();

    std::function<void()> on_closed;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    int search_flags() const;
    void scope(sptr_t* start, sptr_t* end) const;
    bool find_in(sptr_t from, sptr_t to, sptr_t* match_start, sptr_t* match_end) const;
    bool find_wrapping(bool is_forward, sptr_t from, sptr_t* match_start, sptr_t* match_end) const;
    QString pattern_problem() const;
    void seed_query();
    void search_from_anchor();
    void refresh_matches();
    void clear_matches();
    void select_match(sptr_t start, sptr_t end);
    void select_all_matches();
    sptr_t replace_target(sptr_t start, sptr_t end);
    void replace_current();
    void replace_all();
    void set_replace_shown(bool is_shown);
    void remember_query();
    void recall_query(int direction);

    QPointer<ScintillaEditBase> m_view;
    field_frame* m_query_field;
    QLineEdit* m_query;
    QToolButton* m_case;
    QToolButton* m_word;
    QToolButton* m_regex;
    QToolButton* m_in_selection;
    QLabel* m_count;
    QToolButton* m_twisty;
    QWidget* m_replace_row;
    field_frame* m_replace_field;
    QLineEdit* m_replacement;
    QToolButton* m_preserve_case;
    QTimer* m_replaced_note;
    sptr_t m_anchor = 0;
    sptr_t m_scope_start = 0;
    sptr_t m_scope_end = 0;
    QStringList m_history;
    int m_history_index = 0;
};

#endif
