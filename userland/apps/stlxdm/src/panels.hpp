#ifndef STLXDM_PANELS_HPP
#define STLXDM_PANELS_HPP

#include "damage.hpp"
#include "picture.hpp"
#include "shell/bar.hpp"
#include "shell/direct_host.hpp"
#include "shell/popover.hpp"
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
 * launchers with hover tooltips. Both are toolkit trees painted straight
 * into the shared backdrop, so composing them is the same blit as the
 * wallpaper. The bar's targets open popover panels above the windows. */
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

    /* Paints into `backdrop` and puts popover panels on glass from `frost` from here on. Both must have the
     * size and hold the pixels of the ones before, and `frost` must stay valid until the next `rebind()` or
     * `shutdown()`. */
    void rebind(stlxgfx_surface_t* backdrop, const stlxgfx_surface_t* frost);

    /* Fired by a dock pin's release, the server spawns the app */
    std::function<void(const stlxconf_pin_t&)> on_launch;

    /* Fired by a system menu item or a stats panel link: a program to spawn with its args, or the power overlay */
    std::function<void(const char* path, const char* args)> on_spawn;
    std::function<void()> on_power;

    bool dirty() const {
        return m_host.dirty() || m_dock_host.dirty() || m_popover.dirty();
    }

    int32_t dock_y() const { return m_dock_y; }
    int32_t dock_h() const { return m_dock_h; }

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
     * is open. A pinned one leaves Esc to the hotkeys and the focused window. */
    bool escape_closes_popover() const { return m_popover.panel() && !m_popover.panel()->pinned(); }

    bool popover_contains(int32_t x, int32_t y) const { return m_popover.contains(x, y); }
    void close_popover() { m_popover.close(); }
    void close_popover_now() { m_popover.close_now(); }

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

        if (x < 0 || x >= static_cast<int32_t>(m_width)) {
            return false;
        }

        return (y >= 0 && y < BAR_H) || (y >= m_dock_y && y < static_cast<int32_t>(m_height));
    }

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
    void hover_pin(int32_t index, bool entered);
    damage_list::rect pin_icon_rect(int32_t index) const;
    bool find_tooltip(damage_list::rect& tip, const char*& label) const;

    const stlxconf_t* m_conf = nullptr;
    const stlxgfx_surface_t* m_frost = nullptr;  /* the main output's, which popover panels' glass is drawn from */
    direct_host m_host;
    direct_host m_dock_host;

    /* Views into the backdrop rows each tree paints, and the glass
     * each tree paints beneath its widgets */
    stlxgfx_surface_t* m_band = nullptr;
    stlxgfx_surface_t* m_dock = nullptr;
    stlxgfx_surface_t* m_bar_glass = nullptr;
    stlxgfx_surface_t* m_dock_glass = nullptr;
    std::vector<stlxgfx_surface_t*> m_icons;
    stlxgfx_font* m_tip_font = nullptr;
    stlxgfx_font_metrics m_tip_fm = {};
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int32_t m_dock_y = 0;
    int32_t m_dock_h = 0;

    /* The bar's widgets, which the host's tree owns */
    top_bar m_bar;

    /* The sampler the CPU and memory readouts show, which the server owns */
    stats_sampler* m_stats = nullptr;

    /* The network as last queried, and when. It is queried at a slower cadence than the stats. */
    network_summary m_network;
    uint64_t m_net_query_ns = 0;

    /* The popover host under the bar, and the popover panels the bar's targets open */
    popover_host m_popover;
    std::unique_ptr<system_menu> m_menu;
    std::unique_ptr<stats_panel> m_stats_panel;

    /* Dock pin under the pointer, -1 outside, drives the tooltip */
    int32_t m_hover_pin = -1;
    int32_t m_drawn_hover_pin = -1;
};

#endif
