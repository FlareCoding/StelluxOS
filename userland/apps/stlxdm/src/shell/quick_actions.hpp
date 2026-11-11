#ifndef STLXDM_SHELL_QUICK_ACTIONS_HPP
#define STLXDM_SHELL_QUICK_ACTIONS_HPP

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

/* The palette Super Space opens over the windows: a field that ranks launchers, actions, programs no launcher names
 * and the files the index found by what is typed, and works out arithmetic, where Enter starts a row */
class quick_actions : public popover_panel {
public:
    /* Lists the launchers and actions of `conf`, the launchers with `launcher_icons`, one per launcher and null
     * where none loaded. Both must outlive the palette. */
    quick_actions(const stlxconf_t& conf, const std::vector<stlxgfx_surface_t*>& launcher_icons);
    ~quick_actions() override;

    quick_actions(const quick_actions&) = delete;
    quick_actions& operator=(const quick_actions&) = delete;

    /* Fired by a chosen row: a launcher to start as the dock does, an action, a program to run in Terminal with its
     * space separated args, or a value to copy. All must be set before the palette opens. */
    std::function<void(const stlxconf_pin_t& pin)> on_launch;
    std::function<void(const stlxconf_action_t& action)> on_run;
    std::function<void(const char* program, const char* args)> on_run_program;
    std::function<void(const std::string& text)> on_copy;

    /* Fired by a chosen file or folder: a program to run with exactly these args, its handler or File Manager */
    std::function<bool(const char* program, const std::vector<std::string>& args)> on_exec;

    /* Ticks the rows of the launchers whose bit is set in `running`, from the next opening on */
    void show_running(uint64_t running) { m_running = running; }

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

    const stlxconf_t&                      m_conf;
    const std::vector<stlxgfx_surface_t*>& m_launcher_icons;

    /* The icons the actions' files name, one per action and null where it names none or none loaded */
    std::vector<stlxgfx_surface_t*> m_action_icons;

    /* This session's last picks, newest first, each as the launcher, action, program or file it started, and the
     * files among them as found */
    std::vector<std::string> m_picks;
    std::vector<found_file>  m_picked_files;

    /* The files the palette searches, and the open palette's taker of their answers, unset while it is closed */
    file_index                                                          m_files;
    std::function<void(const file_answer* answer, const walk_state& walk)> m_show_files;

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
