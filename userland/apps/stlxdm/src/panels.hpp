#ifndef STLXDM_PANELS_HPP
#define STLXDM_PANELS_HPP

#include "damage.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>

/* The compositor's private host: a widget tree painted into a band
 * of the screen, events fed by the display manager's own router.
 * This is the proof the toolkit does not depend on the protocol. */
class direct_host : public ui::host {
public:
    void flush() override {}

    bool dirty() const { return tree_dirty(); }
    void layout_now(int32_t w, int32_t h) { layout_tree({ w, h }); }
    void paint_now(void* surface, std::vector<ui::rect>& damage_out) {
        paint_tree(surface, damage_out);
    }

    void pointer_move(int32_t x, int32_t y) {
        dispatch_pointer_move({ x, y });
    }
    void pointer_button(int32_t x, int32_t y, uint8_t btn, bool down) {
        dispatch_pointer_button({ x, y }, btn, down);
    }
};

/* The compositor's chrome: a top bar with the product name, system
 * stats, the clock, and network state, and a bottom dock of pinned
 * launchers with hover tooltips. Both are toolkit trees painted
 * straight into the shared backdrop, so composing them is the same
 * blit as the wallpaper. */
class dm_panels {
public:
    static constexpr int32_t BAR_H = 28;

    /* Builds both panels over the backdrop, which must already hold the
     * wallpaper, each painting its widgets over a picture of its glass
     * drawn from `frost`, the frost of that wallpaper */
    int init(uint32_t screen_w, uint32_t screen_h, const stlxconf_t& conf,
             stlxgfx_surface_t* backdrop, const stlxgfx_surface_t* frost);
    void shutdown();

    /* Paints into `backdrop` from here on, which must have the size and
     * hold the pixels of the backdrop the panels painted into until now */
    void rebind(stlxgfx_surface_t* backdrop);

    /* Fired by a dock pin's release, the server spawns the app */
    std::function<void(const stlxconf_pin_t&)> on_launch;

    bool dirty() const {
        return m_host.dirty() || m_dock_host.dirty();
    }

    int32_t dock_y() const { return m_dock_y; }
    int32_t dock_h() const { return m_dock_h; }

    /* Paints dirty panel subtrees into the backdrop and adds the
     * changed regions, tooltip transitions included, to the screen
     * damage list. */
    void flush(damage_list& damage);

    /* Draws the hover tooltip above the dock, over the windows. */
    void compose_top(stlxgfx_surface_t* back, const damage_list::rect& r);

    /* The bounds `compose_top` draws inside, empty without a tooltip */
    damage_list::rect top_bounds() const;

    bool contains(int32_t x, int32_t y) const {
        if (x < 0 || x >= static_cast<int32_t>(m_width)) {
            return false;
        }

        return (y >= 0 && y <= BAR_H) || (y >= m_dock_y && y < static_cast<int32_t>(m_height));
    }

    void pointer_move(int32_t x, int32_t y);
    void pointer_button(int32_t x, int32_t y, uint8_t btn, bool down);

    /* Nanoseconds until the next second rollover, and the tick that
     * applies it. The clock is the desktop's only idle timer. */
    int64_t clock_timeout_ns(uint64_t now_ns) const;
    void clock_tick();

private:
    void hover_pin(int32_t index, bool entered);
    damage_list::rect pin_icon_rect(int32_t index) const;
    bool find_tooltip(damage_list::rect& tip, const char*& label) const;

    const stlxconf_t* m_conf = nullptr;
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
    ui::label* m_clock = nullptr;
    ui::label* m_stats = nullptr;
    ui::label* m_net = nullptr;

    /* Last query times gating the refresh cadences */
    uint64_t m_stats_query_ns = 0;
    uint64_t m_net_query_ns = 0;

    /* Dock pin under the pointer, -1 outside, drives the tooltip */
    int32_t m_hover_pin = -1;
    int32_t m_drawn_hover_pin = -1;
};

#endif
