#ifndef SHEEN_SHEET_H
#define SHEEN_SHEET_H

/* A modal panel inside Sheen's window over a dimmed scrim. Stellux gives a separate dialog window no tie to its
 * owner, so it could hide behind Sheen while blocking it, and a sheet never leaves the window. */
#include <QList>
#include <QWidget>

class QEventLoop;
class QPushButton;

class sheet : public QWidget {
public:
    sheet(QWidget* window, int panel_width);
    ~sheet() override;

    /* Shows the sheet and blocks, running the event loop, until `finish` closes it. Returns the result passed to
     * `finish`, 0 when cancelled */
    int run();
    void finish(int result);

    QWidget* panel() const { return m_panel; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool focusNextPrevChild(bool next) override;

    /* Esc cancels, unless a sheet gives it an earlier step such as clearing a filter */
    virtual void cancel();

    /* Fixes the panel's height, which otherwise follows its layout, kept within the window */
    void set_panel_height(int height);
    void set_focus_target(QWidget* widget) { m_focus_target = widget; }

private:
    void place();

    QWidget* m_panel;
    QWidget* m_focus_target = nullptr;
    QEventLoop* m_loop = nullptr;
    int m_result = 0;
    int m_panel_width;
    int m_panel_height = 0;
};

/* The answers a prompt offers: the primary action at the right with Cancel beside it, and an optional destructive
 * action alone at the left that only a click or Ctrl D gives, so no single letter throws work away */
enum class prompt_answer { CANCEL, PRIMARY, DESTRUCTIVE };

class prompt_sheet : public sheet {
public:
    prompt_sheet(QWidget* window, const QString& title, const QString& body, const QString& primary,
                 const QString& destructive, bool is_primary_amber);

    prompt_answer ask();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QPushButton* m_destructive = nullptr;
};

/* One unsaved document in a checklist */
struct unsaved_entry {
    QString name;
    QString place; // its folder, or how a never-saved file gets one
};

/* Several unsaved documents at once, each with a check box: the primary action saves the checked ones and discards
 * the rest, and Discard All discards every one */
class checklist_sheet : public sheet {
public:
    checklist_sheet(QWidget* window, const QString& title, const QString& body, const QList<unsaved_entry>& entries);

    /* Returns the answer, and for PRIMARY which entries stay checked */
    prompt_answer ask(QList<bool>* checked);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void refresh_primary();

    QList<bool> m_checked;
    QPushButton* m_primary;
};

#endif
