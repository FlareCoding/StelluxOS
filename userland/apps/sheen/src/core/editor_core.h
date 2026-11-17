#ifndef SHEEN_CORE_EDITOR_CORE_H
#define SHEEN_CORE_EDITOR_CORE_H

/* Sheen's editing surface: the tab strip, a banner about the current file, find and replace, the document stack, and
 * the window's status bar. Composed inside `sheen_window` directly or beside `workspace_shell` without recreating views. */
#include "app/settings.h"
#include "core/actions.h"
#include "core/document.h"
#include "extensions/extension_host.h"
#include "ui/sheet.h"
#include "ui/status_bar.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

class QAction;
class QMainWindow;
class QMenu;
class QStackedWidget;
class QUrl;
class banner;
class empty_state;
class find_bar;
class picker;
struct picker_row;
class menu_style;
class sheet;
class tab_row;
class tab_strip;
class tab_switcher;
struct load_result;
struct recent_file;

namespace sheen {

class sheen_app;

/* A closed tab, enough to open it again where it was */
struct closed_tab {
    file_record file;
    const language* lang;
    view_state view;
    int index;
    int untitled_number;
    sptr_t scintilla_document; // kept with its undo for unsaved or never-saved text, 0 to read the file again
};

/* Where the caret stood in a tab before a jump, which Back and Forward return to */
struct caret_place {
    document* doc;
    sptr_t line;
    sptr_t column;

    bool operator==(const caret_place&) const = default;
};

struct bound_extension_command {
    QString owner_id;
    extension_command_spec spec;
    QAction* action = nullptr;
};

struct migrated_tab {
    file_record file;
    const language* lang = nullptr;
    view_state view;
    int untitled_number = 1;
    sptr_t scintilla_document = 0;
    bool is_deleted = false;
    bool is_modified = false;
};

class editor_core : public QWidget {
public:
    editor_core(QMainWindow* window, sheen_app& app, QWidget* parent = nullptr);
    ~editor_core() override;

    /* Opens each path in a tab. A path that does not exist yet opens as a new file its first save creates, and a
     * folder opens the Open sheet there once the window shows */
    void open_paths(const QStringList& paths);
    bool open_path(const QString& path, const QByteArray& encoding, int index);
    void new_document();

    bool take_file_drag(QEvent* event);
    bool has_open_sheet() const { return m_open_sheet != nullptr; }
    bool confirm_close(const QString& purpose);
    void handle_window_activated();
    bool handle_escape_key();
    void apply_settings(const sheen_settings& settings, const QStringList& problems);
    void refresh_empty_state_if_shown();

    status_bar* status_widget() const { return m_status; }
    tab_row* tab_row_widget() const { return m_tab_row; }
    int document_count() const { return static_cast<int>(m_documents.size()); }
    QString active_file_path() const;
    QString active_tab_title() const;
    QList<diagnostic_item> current_diagnostics() const { return all_diagnostics(); }
    bool focus_if_open(const QString& path);
    bool is_untitled_number_taken(int number) const;
    void set_workspace_root(const QString& root_path);

    void show_welcome_state();
    void focus_active_surface();
    bool can_promote_in_place_for(const QString& root_path) const;
    bool has_modified_document(const QString& path) const;
    void drop_pristine_untitled_tabs();
    QList<migrated_tab> extract_tabs_under(const QString& root_path, const editor_core* destination = nullptr);
    void adopt_migrated_tabs(const QList<migrated_tab>& migrated);

    void open_custom_tab(const QString& owner_id, const custom_tab_spec& tab);
    QWidget* focus_custom_tab(const QString& owner_id, const QString& tab_id);
    void close_custom_tabs(const QString& owner_id);
    void set_diagnostics(const QString& owner_id, const QList<diagnostic_item>& diagnostics);
    void clear_diagnostics(const QString& owner_id);
    void set_line_badges(const QString& owner_id, const QList<line_badge_item>& badges);
    void clear_line_badges(const QString& owner_id);
    void set_extension_commands(const QString& owner_id, const QList<extension_command_spec>& commands);
    void clear_extension_commands(const QString& owner_id);
    bool save_modified_files();
    void open_file_at_line(const QString& path, int line, int column = 0);
    void show_extension_status_note(const QString& note);

    std::function<void(const QString& title)> on_title_changed;
    std::function<void(const QString& file_path)> on_active_file_changed;
    std::function<void()> on_diagnostics_changed;
    std::function<void(const QString& file_path)> on_file_saved;
    std::function<void(const QString& root_path)> on_open_folder_requested;

protected:
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
    void notify_title_changed(const QString& title);
    void switch_window();

    document* add_document(std::unique_ptr<document> owned, int index);
    document* current_document() const;
    int index_of(const document* doc) const;
    document* find_open(const QString& path) const;
    tab_strip* tabs() const;
    int insertion_index() const;
    int lowest_free_untitled_number() const;
    void remember_recent_file(const QString& path);
    QList<recent_file> recent_files() const;

    void open_dropped(const QList<QUrl>& urls);
    void choose_files_to_open(const QString& folder);
    void choose_folder_to_open(const QString& start_folder);
    void handle_top_bar_open_folder();
    void sync_open_folder_button();
    bool load_in_background(const QString& path, qint64 size, const QByteArray& encoding, int index);
    void finish_loading(document* doc, const load_result& result);
    void hold_binary(document* doc, const QString& path, const QByteArray& bytes);
    bool save_document(document* doc);
    bool save_document_as(document* doc, const QString& title = QString());
    bool write_document(document* doc, const QString& path);
    bool save_documents(const QList<document*>& docs, const QString& purpose);
    void revert_document(document* doc);
    bool close_document(int index);
    bool close_documents(const QList<document*>& docs, const QString& purpose, bool closes_tabs = true);
    prompt_answer ask_about_unsaved(document* doc, const QString& purpose);
    void remove_document(int index, bool keeps_text, bool records_closed = true);
    void reopen_closed_tab();
    void check_disk(document* doc);
    void select_tab(int index);
    void show_empty_state();
    void set_zoom(int zoom);
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
    void go_to_line(bool is_from_palette);
    void record_jump(document* doc);
    void step_jump(bool is_back);
    void open_palette(const QString& query);
    document* settings_document() const;
    void open_settings(const QString& section);
    bool save_settings();
    void take_settings_from_text();
    void reread_settings();
    void show_settings_problems(document* doc);
    void fill_palette(const QString& query);
    void list_palette_actions(QList<picker_row>* rows, QList<std::function<void()>>* targets);
    void list_palette_files(QList<picker_row>* rows, QList<std::function<void()>>* targets);
    void fill_indentation_menu(document* doc, QMenu* menu);
    void fill_encoding_menu(document* doc, QMenu* menu);
    void fill_line_ending_menu(document* doc, QMenu* menu);
    void show_text_menu(const QPoint& anchor);
    void show_tab_menu(int index, const QPoint& anchor);
    void pick_encoding(command id);
    void reopen_with_encoding(const QByteArray& name);
    void save_with_encoding(const file_encoding& encoding);
    void show_about();
    void apply_annotations_to_document(document* doc);
    void apply_annotations_to_all_documents();
    QList<diagnostic_item> all_diagnostics() const;
    void step_problem(bool is_next);
    void rebuild_extension_menus();
    QString sheet_folder() const;

    QMainWindow* m_window;
    sheen_app& m_app;
    sheen_settings m_settings;
    QString m_workspace_root;
    tab_row* m_tab_row;
    banner* m_banner;
    find_bar* m_find_bar = nullptr;
    QStackedWidget* m_stack;
    picker* m_picker = nullptr;
    empty_state* m_empty_state;
    status_bar* m_status;
    menu_style* m_menu_style;
    QMenu* m_main_menu;
    QList<QMenu*> m_menus;
    QList<QMenu*> m_extension_menus;
    QList<bound_extension_command> m_extension_commands;
    QMap<QString, QList<diagnostic_item>> m_diagnostics_by_owner;
    QMap<QString, QList<line_badge_item>> m_line_badges_by_owner;
    int m_problem_cursor = -1;
    std::vector<std::unique_ptr<document>> m_documents;
    QList<document*> m_recent_tabs;
    QList<closed_tab> m_closed_tabs;
    QStringList m_recent_encodings;
    int m_zoom = 0;
    QList<command> m_recent_commands;
    QList<caret_place> m_back_places;
    QList<caret_place> m_forward_places;
    bool m_is_stepping_jumps = false;
    QStringList m_settings_problems;
    QList<std::function<void()>> m_palette_targets;
    QString m_last_sheet_folder;
    QHash<command, QAction*> m_actions;
    sheet* m_open_sheet = nullptr;
    tab_switcher* m_switcher;
    bool m_is_switching_tabs = false;
    bool m_has_opened_any_tab = false;
    int m_switch_index = 0;
    QString m_status_note;
};

} // namespace sheen

#endif
