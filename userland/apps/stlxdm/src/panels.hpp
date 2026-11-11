#ifndef STLXDM_PANELS_HPP
#define STLXDM_PANELS_HPP

#include "damage.hpp"
#include "picture.hpp"
#include "shell/bar.hpp"
#include "shell/direct_host.hpp"
#include "shell/dock.hpp"
#include "shell/popover.hpp"
#include "shell/quick_actions.hpp"
#include "shell/stats.hpp"
#include "shell/stats_panel.hpp"
#include "shell/system_menu.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>
#include <stlxtheme/theme.h>
#include <stlxui/stlxui.h>

#include <cstdint>

/* The compositor's chrome: a top bar and a bottom dock of pinned
 * launchers with hover tooltips and running ticks. Both are toolkit trees
 * painted straight into the shared backdrop, so composing them is the same
 * blit as the wallpaper. The bar's targets open popover panels above the
 * windows. */
class dm_panels {
public:
    static constexpr int32_t BAR_H = STLXTHEME_TOP_BAR_HEIGHT;

    /* Builds the bar, the dock, the system menu and the stats panel on glass from the main output's `frost`,
     * which must stay valid until `rebind()` replaces it. The bar and the dock paint over `backdrop`, which must
     * already hold the wallpaper. The bar, the menu and the stats panel show `stats`, and the menu reads its
     * shortcuts from `conf` at each opening, so both must outlive the panels. Returns -1 when a view, a picture
     * or the tooltip font cannot be made, after which the caller must still call `shutdown()`. */
    int init(
        uint32_t screen_w,
        uint32_t screen_h,
        const stlxconf_t& conf,
        stlxgfx_surface_t* backdrop,
        const stlxgfx_surface_t* frost,
        stats_sampler& stats
    );
    void shutdown();

    /* Paints into `backdrop` and puts popover panels and the tooltip on glass from `frost` from here on. Both
     * must have the size and hold the pixels of the ones before, and `frost` must stay valid until the next
     * `rebind()` or `shutdown()`. */
    void rebind(stlxgfx_surface_t* backdrop, const stlxgfx_surface_t* frost);

    /* Fired first by a launcher's release, true when the server brought back a minimized window of the app */
    std::function<bool(const stlxconf_pin_t&)> on_unminimize;

    /* Fired by a launcher's release when `on_unminimize` is unset or returned false: the server spawns the app */
    std::function<void(const stlxconf_pin_t&)> on_launch;

    /* Fired by a system menu item or a stats panel link: a program to spawn with its args, or the power overlay */
    std::function<void(const char* path, const char* args)> on_spawn;
    std::function<void()> on_power;

    /* Fired by Quick Actions' calculator: text to put on the clipboard */
    std::function<void(const std::string& text)> on_copy;

    /* Fired by a file Quick Actions opens: a program to run with exactly these args, true when it started */
    std::function<bool(const char* program, const std::vector<std::string>& args)> on_exec;

    bool dirty() const {
        return m_host.dirty() || m_dock_host.dirty() || m_popover.dirty();
    }

    /* The top row of the dock's shelf, where the windows' work area ends whether or not the dock has launchers */
    int32_t dock_y() const { return m_dock_layout.shelf.y; }

    /* The tile in desktop coordinates of the launcher at `index`, empty
     * past the last launcher */
    damage_list::rect launcher_tile(uint32_t index) const;

    /* The index of the first launcher that starts the app `app_id`, which must not be null, or -1 when none does */
    int32_t launcher_of(const char* app_id) const;

    /* Ticks each launcher whose bit is set in `running`, the one at `focused` longer. `focused` is the launcher
     * whose app holds the focus, or -1 for none. */
    void show_running_launchers(uint64_t running, int32_t focused);

    /* Paints the bar's and the dock's dirty subtrees into the backdrop, adding the regions they changed
     * to `backdrop_damage`, and adds the regions the popover and the tooltip changed above the windows to
     * `top_damage` */
    void flush(damage_list& backdrop_damage, damage_list& top_damage);

    /* The shown popover, a closing one included, as both compositors blend it above the windows */
    popover_layer popover() const { return m_popover.layer(); }

    /* Makes popover pictures in `maker`'s memory, or the CPU's own when
     * null, closing a shown popover at once */
    void use_picture_maker(picture_maker* maker) { m_popover.use_picture_maker(maker); }

    /* Whether Esc closes the popover, as it does while an unpinned popover panel
     * is open. A pinned one leaves Esc to the focused window. */
    bool escape_closes_popover() const { return m_popover.panel() && !m_popover.panel()->pinned(); }

    /* Whether the open popover panel takes the keyboard, so no key reaches the focused window while it is open */
    bool popover_takes_keys() const { return m_popover.panel() && m_popover.panel()->takes_keys(); }

    /* Offers a key to the open popover panel, true when its tree took it */
    bool popover_key(const ui::key_event& e, bool down) { return m_popover.key(e, down); }

    bool popover_contains(int32_t x, int32_t y) const { return m_popover.contains(x, y); }
    void close_popover() { m_popover.close(); }
    void close_popover_now() { m_popover.close_now(); }

    /* Opens Quick Actions in place of any popover, or closes it when it is open */
    void toggle_quick_actions() { toggle_popover(*m_quick); }

    /* Readable while Quick Actions' file index has news, which `pump_quick_files()` takes */
    int quick_files_fd() const { return m_quick ? m_quick->files_fd() : -1; }
    void pump_quick_files() { m_quick->pump_files(); }

    /* Whether a press at (`x`, `y`) closes the popover: one outside an open
     * or opening popover that is not pinned, and outside the bar's targets */
    bool dismisses_popover(int32_t x, int32_t y) const;

    /* Draws the hover tooltip above the dock, over the windows. */
    void compose_top(stlxgfx_surface_t* back, const damage_list::rect& r);

    /* The bounds `compose_top` draws inside, empty without a tooltip */
    damage_list::rect top_bounds() const;

    /* Whether the bar, the dock or an open or opening popover lies under (`x`, `y`) */
    bool contains(int32_t x, int32_t y) const {
        if (m_popover.contains(x, y)) {
            return true;
        }

        const damage_list::rect& shelf = m_dock_layout.shelf;
        bool on_shelf = x >= shelf.x && x < shelf.x + shelf.w && y >= shelf.y && y < shelf.y + shelf.h;
        bool on_bar = x >= 0 && x < static_cast<int32_t>(m_width) && y >= 0 && y < BAR_H;

        return on_bar || on_shelf;
    }

    /* Whether a launcher's tile took a press, so the pointer's motion and buttons must come here until its release */
    bool holds_pointer() const { return m_dock_host.holds_pointer(); }

    void pointer_move(int32_t x, int32_t y);
    void pointer_button(int32_t x, int32_t y, uint8_t btn, bool down);

    /* The desktop's only idle timer: nanoseconds until the stats sampler's
     * next period starts, and the tick that samples the stats and updates
     * the bar */
    int64_t clock_timeout_ns(uint64_t now_ns) const;
    void clock_tick();

    /* Shows beside the mark the label of the first labeled launcher whose
     * program has id `app_id`, else the id, and nothing without an id */
    void show_focused_app(const char* app_id);

private:
    void toggle_popover(popover_panel& panel);
    void switch_popover(popover_panel& panel);
    void sync_bar_targets();
    void launch(const stlxconf_pin_t& pin);
    void run_action(const stlxconf_action_t& action);
    void run_in_terminal(const char* program, const char* args);
    void hover_pin(int32_t index, bool entered);
    bool find_tooltip(damage_list::rect& tip, const char*& label) const;

    const stlxconf_t* m_conf = nullptr;
    const stlxgfx_surface_t* m_frost = nullptr;  /* the main output's, which popover and tooltip glass is drawn from */
    direct_host m_host;
    direct_host m_dock_host;

    /* Views into the backdrop rows each tree paints, and the glass
     * the bar paints beneath its widgets */
    stlxgfx_surface_t* m_band = nullptr;
    stlxgfx_surface_t* m_dock = nullptr;
    stlxgfx_surface_t* m_bar_glass = nullptr;

    /* The icons the dock's tiles and Quick Actions draw, one per launcher and null where none loaded */
    std::vector<stlxgfx_surface_t*> m_icons;

    /* The dock's tiles, which its tree owns, and where its shelf and band lie */
    std::vector<dock_tile*> m_tiles;
    dock_layout m_dock_layout;

    stlxgfx_font* m_tip_font = nullptr;
    stlxgfx_font_metrics m_tip_fm = {};
    uint32_t m_width = 0;

    /* The bar's widgets, which the host's tree owns */
    top_bar m_bar;

    /* The sampler the CPU and memory readouts show, which the server owns */
    stats_sampler* m_stats = nullptr;

    /* The network as last queried, and when. It is queried at a slower cadence than the stats. */
    network_summary m_network;
    uint64_t m_net_query_ns = 0;

    /* The popover host, the popover panels the bar's targets open, and Quick Actions, which a shortcut opens */
    popover_host m_popover;
    std::unique_ptr<system_menu> m_menu;
    std::unique_ptr<stats_panel> m_stats_panel;
    std::unique_ptr<quick_actions> m_quick;

    /* Dock pin under the pointer, -1 outside, drives the tooltip */
    int32_t m_hover_pin = -1;
    int32_t m_drawn_hover_pin = -1;
};

#endif
