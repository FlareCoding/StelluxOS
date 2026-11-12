#ifndef STLXDM_SHELL_QUICK_ACTIONS_HPP
#define STLXDM_SHELL_QUICK_ACTIONS_HPP

#include "../clipboard.hpp"
#include "file_index.hpp"
#include "popover.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* One thing a row can do, as Ctrl K lists it: a row's first runs on Enter, and `keys`, space separated, name the
 * chord that runs another, none for one only the list runs */
struct row_action {
    std::string           label;
    std::string           keys;
    std::function<void()> run;
};

/* The palette Super Space opens over the windows, ranking launchers, actions, programs and files by what is typed and
 * working out arithmetic, and Super V opens in Clipboard mode over the clipboard history */
class quick_actions : public popover_panel {
public:
    /* Lists the launchers and actions of `conf`, the launchers with `launcher_icons`, one per launcher and null
     * where none loaded. Both must outlive the palette. */
    quick_actions(const stlxconf_t& conf, const std::vector<stlxgfx_surface_t*>& launcher_icons);
    ~quick_actions() override;

    quick_actions(const quick_actions&) = delete;
    quick_actions& operator=(const quick_actions&) = delete;

    /* Fired by a chosen row: a launcher to start as the dock does, an action, a program to run in Terminal with its
     * space separated args, a value to copy, or a file to copy as a file. All must be set before the palette opens. */
    std::function<void(const stlxconf_pin_t& pin)> on_launch;
    std::function<void(const stlxconf_action_t& action)> on_run;
    std::function<void(const char* program, const char* args)> on_run_program;
    std::function<void(const std::string& text)> on_copy;
    std::function<void(const std::string& path)> on_copy_file;

    /* Fired by a chosen file or folder: a program to run with exactly these args, its handler or File Manager */
    std::function<bool(const char* program, const std::vector<std::string>& args)> on_exec;

    /* Fired by a chosen clipboard entry: one to paste into the focused window, or one to make current */
    std::function<void(uint64_t id)> on_clipboard_paste;
    std::function<void(uint64_t id)> on_clipboard_copy;

    /* Ticks the rows of the launchers whose bit is set in `running`, from the next opening on */
    void show_running(uint64_t running) { m_running = running; }

    /* The history Clipboard mode lists, which must outlive the palette, and when it began, such as 09:58 */
    void use_clipboard(const clipboard_history* history, std::string since);

    /* The label of the app a paste lands in, empty with no window focused */
    void show_focused_app(std::string label) { m_focused_app = std::move(label); }

    /* Opens the palette in Clipboard mode from the next opening on, or in Quick Actions */
    void open_in_clipboard(bool is_clipboard) { m_opens_clipboard = is_clipboard; }

    /* Whether the open palette shows Clipboard mode, switching it there, and showing the history again */
    bool shows_clipboard() const;
    void show_clipboard();
    void clipboard_changed();

    /* Readable while the file index has an answer or news of its walk */
    int files_fd() const { return m_files.wake_fd(); }

    /* Hands the file index's newest answer and its walk to the open palette */
    void pump_files();

    int32_t width() const override;
    int32_t left(int32_t output_w) const override;
    int32_t top() const override;
    bool takes_keys() const override { return true; }
    std::unique_ptr<ui::widget> build() override;
    void closed() override;

private:
    void remember_pick(const std::string& key);
    void remember_file(const found_file& file);
    void open_file(const std::string& path, const char* mime_type);
    void open_with(const stlxconf_file_handler_t& handler, const std::string& path);
    std::string file_verb(const char* mime_type) const;
    std::string file_note(const found_file& file) const;
    std::vector<row_action> launcher_actions(uint32_t index, const std::string& pick);
    std::vector<row_action> action_actions(uint32_t index, const std::string& pick);
    std::vector<row_action> program_actions(const std::string& path, const std::string& args, const std::string& pick);
    std::vector<row_action> file_actions(const found_file& file, const std::string& pick);
    row_action show_in_files(const std::string& path);
    row_action copy(const std::string& label, const std::string& text);
    bool is_startable(const char* verb, const std::string& label, const char* path, const char* owner,
                      const char* source_path);
    bool is_still_there(const found_file& file);

    const stlxconf_t&                      m_conf;
    const std::vector<stlxgfx_surface_t*>& m_launcher_icons;

    /* The icons the actions' files name, one per action and null where it names none or none loaded */
    std::vector<stlxgfx_surface_t*> m_action_icons;

    /* This session's last picks, newest first, each as the launcher, action, program or file it started, and the
     * files among them as found */
    std::vector<std::string> m_picks;
    std::vector<found_file>  m_picked_files;

    /* The files the palette searches, and the open palette's takers of their answers and of a failure to start a
     * row, unset while it is closed */
    file_index                                                             m_files;
    std::function<void(const file_answer* answer, const walk_state& walk)> m_show_files;
    std::function<void(const std::string& message, const std::string& source_path, const std::string& gone_path)>
        m_show_failure;

    /* The history Clipboard mode lists, when it began, where a paste lands, and the open palette's hooks into
     * Clipboard mode, unset while it is closed */
    const clipboard_history* m_history = nullptr;
    std::string              m_history_since;
    std::string              m_focused_app;
    bool                     m_opens_clipboard = false;
    std::function<bool()>    m_shows_clipboard;
    std::function<void()>    m_show_clipboard;
    std::function<void()>    m_refresh_clipboard;

    /* Each chip's glyph at rest, then each chip's when chosen */
    std::vector<stlxgfx_surface_t*> m_chip_glyphs;

    stlxgfx_surface_t* m_search_glyph = nullptr;
    stlxgfx_surface_t* m_program_icon = nullptr;
    stlxgfx_surface_t* m_folder_icon = nullptr;
    stlxgfx_surface_t* m_text_icon = nullptr;
    stlxgfx_surface_t* m_image_icon = nullptr;
    uint64_t           m_running = 0;
};

#endif
