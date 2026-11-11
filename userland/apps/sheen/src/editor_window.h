#ifndef SHEEN_EDITOR_WINDOW_H
#define SHEEN_EDITOR_WINDOW_H

/* Sheen's window: the menu button, a tab per document, a banner about the current file, its text and the status
 * bar. Commands arrive through `run`, questions are sheets inside the window, and closed tabs can come back. */
#include "actions.h"
#include "document.h"
#include "settings.h"
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
class find_bar;
class menu_style;
class sheet;
class tab_row;
class tab_strip;
class tab_switcher;
struct load_result;

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
    /* `problems` are what reading the settings found, shown in the status bar */
    editor_window(const sheen_settings& settings, const QStringList& problems);
    ~editor_window() override;

    /* Opens each path in a tab. A path that does not exist yet opens as a new file its first save creates, and a
     * folder opens the Open sheet there once the window shows */
    void open_paths(const QStringList& paths);

    /* Paths a later launch handed over. The window comes forward, and an open sheet stays with its tabs behind it */
    void receive_paths(const QStringList& paths);

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    bool load_in_background(const QString& path, qint64 size, const QByteArray& encoding, int index);
    void finish_loading(document* doc, const load_result& result);
    void hold_binary(document* doc, const QString& path, const QByteArray& bytes);
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
    void step_recent_tab(int step);
    void finish_tab_switch(bool opens_lit_tab);
    void show_tab_list();
    void close_saved_tabs();

    void show_current_document();
    void refresh_tab(document* doc);
    void refresh_tab_names();
    void refresh_status();
    void refresh_banner();
    void show_banner(document* doc, const banner_message& message);
    void show_status_note(const QString& note);
    void show_status_menu(status_kind kind, const QPoint& anchor);
    void fill_indentation_menu(document* doc, QMenu* menu);
    void fill_encoding_menu(document* doc, QMenu* menu);
    void fill_line_ending_menu(document* doc, QMenu* menu);
    void show_text_menu(const QPoint& anchor);
    void show_tab_menu(int index, const QPoint& anchor);
    void reopen_with_encoding(const QByteArray& name);
    void save_with_encoding(const QByteArray& name);
    void show_about();
    QString sheet_folder() const;

    sheen_settings m_settings;
    tab_row* m_tab_row;
    banner* m_banner;
    find_bar* m_find_bar = nullptr;
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
    tab_switcher* m_switcher;
    bool m_is_switching_tabs = false;
    int m_switch_index = 0;
    QString m_status_note;
};

#endif
