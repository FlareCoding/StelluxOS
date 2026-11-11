#ifndef SHEEN_EDITOR_WINDOW_H
#define SHEEN_EDITOR_WINDOW_H

/* Sheen's window: the menu button, a tab per document, a banner about the current file, its text and the status
 * bar. Commands arrive through `run`, questions are sheets inside the window, and closed tabs can come back. */
#include "actions.h"
#include "document.h"
#include "sheet.h"
#include "status_bar.h"

#include <QHash>
#include <QList>
#include <QMainWindow>

#include <memory>
#include <vector>

class QAction;
class QLabel;
class QMenu;
class QStackedWidget;
class banner;
class empty_state;
class menu_style;
class sheet;
class tab_row;
class tab_strip;

/* A closed tab, enough to open it again where it was */
struct closed_tab {
    file_record file;
    const language* lang;
    view_state view;
    int index;
    int untitled_number;
    sptr_t scintilla_document; // kept with its undo for unsaved or never-saved text, 0 to read the file again
};

class editor_window : public QMainWindow {
public:
    editor_window();
    ~editor_window() override;

    /* Opens each path in a tab. A path that does not exist yet opens as a new file its first save creates, and a
     * folder opens the Open sheet there once the window shows */
    void open_paths(const QStringList& paths);

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void create_commands();
    void fill_submenu(command id, QMenu* submenu);
    void run(command id);
    void refresh_commands();
    void show_menu(QMenu* menu, bool is_from_keyboard);
    int run_sheet(sheet& open_sheet);
    void set_open_sheet(sheet* open_sheet);
    void focus_current_document();

    document* add_document(std::unique_ptr<document> owned, int index);
    document* current_document() const;
    int index_of(const document* doc) const;
    document* find_open(const QString& path) const;
    tab_strip* tabs() const;
    int insertion_index() const;
    int lowest_free_untitled_number() const;
    void remember_recent_file(const QString& path);

    void new_document();
    void choose_files_to_open(const QString& folder);
    bool open_path(const QString& path, const QByteArray& encoding, int index);
    bool save_document(document* doc);
    bool save_document_as(document* doc, const QString& title = QString());
    bool write_document(document* doc, const QString& path);
    bool save_documents(const QList<document*>& docs, const QString& purpose);
    void revert_document(document* doc);
    bool close_document(int index);
    bool close_documents(const QList<document*>& docs, const QString& purpose);
    prompt_answer ask_about_unsaved(document* doc, const QString& purpose);
    void remove_document(int index, bool keeps_text);
    void reopen_closed_tab();
    void check_disk(document* doc);
    void select_tab(int index);
    void show_empty_state();
    void show_tab_list();
    void close_saved_tabs();

    void show_current_document();
    void refresh_tab(document* doc);
    void refresh_status();
    void refresh_banner();
    void show_banner(document* doc, const banner_message& message);
    void show_status_note(const QString& note);
    void show_fact_menu(file_fact fact, const QPoint& anchor);
    void show_text_menu(const QPoint& anchor);
    void show_tab_menu(int index, const QPoint& anchor);
    void reopen_with_encoding(const QByteArray& name);
    void save_with_encoding(const QByteArray& name);
    void show_about();
    QString sheet_folder() const;

    tab_row* m_tab_row;
    banner* m_banner;
    QStackedWidget* m_stack;
    empty_state* m_empty_state;
    status_bar* m_status;
    menu_style* m_menu_style;
    QMenu* m_main_menu;
    QList<QMenu*> m_menus;
    std::vector<std::unique_ptr<document>> m_documents;
    QList<document*> m_recent_tabs;
    QList<closed_tab> m_closed_tabs;
    QStringList m_recent_files;
    QString m_last_sheet_folder;
    QHash<command, QAction*> m_actions;
    sheet* m_open_sheet = nullptr;
    QString m_status_note;
};

#endif
