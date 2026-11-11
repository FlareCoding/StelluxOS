#ifndef STLXDM_SHELL_QUICK_ACTIONS_HPP
#define STLXDM_SHELL_QUICK_ACTIONS_HPP

#include "popover.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

/* The palette Super Space opens over the windows: a field that ranks the dock's launchers, the configuration's
 * actions and the programs no launcher names by what is typed, where Enter or a click starts the selected row */
class quick_actions : public popover_panel {
public:
    /* Lists the launchers and actions of `conf`, the launchers with `launcher_icons`, one per launcher and null
     * where none loaded. Both must outlive the palette. */
    quick_actions(const stlxconf_t& conf, const std::vector<stlxgfx_surface_t*>& launcher_icons);
    ~quick_actions() override;

    quick_actions(const quick_actions&) = delete;
    quick_actions& operator=(const quick_actions&) = delete;

    /* Fired by a chosen row: a launcher to start as the dock starts it, an action to run, or a program to run in
     * Terminal with its space separated args. All must be set before the palette opens. */
    std::function<void(const stlxconf_pin_t& pin)> on_launch;
    std::function<void(const stlxconf_action_t& action)> on_run;
    std::function<void(const char* program, const char* args)> on_run_program;

    /* Ticks the rows of the launchers whose bit is set in `running`, from the next opening on */
    void show_running(uint64_t running) { m_running = running; }

    int32_t width() const override;
    int32_t left(int32_t output_w) const override;
    int32_t top() const override;
    bool takes_keys() const override { return true; }
    std::unique_ptr<ui::widget> build() override;

private:
    const stlxconf_t&                      m_conf;
    const std::vector<stlxgfx_surface_t*>& m_launcher_icons;

    /* The icons the actions' files name, one per action and null where it names none or none loaded */
    std::vector<stlxgfx_surface_t*> m_action_icons;

    stlxgfx_surface_t* m_search_glyph = nullptr;
    stlxgfx_surface_t* m_program_icon = nullptr;
    uint64_t           m_running = 0;
};

#endif
