#ifndef STLXDM_PANELS_HPP
#define STLXDM_PANELS_HPP

#include "damage.hpp"

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

/* The compositor's chrome: a top bar with the product name, network
 * state, and clock, and a bottom dock of pinned launchers. Both are
 * toolkit trees over retained band surfaces in the background layer. */
class dm_panels {
public:
    static constexpr int32_t BAR_H = 28;
    static constexpr int32_t DOCK_H = 44;

    int init(uint32_t screen_w, uint32_t screen_h);
    void shutdown();

    bool dirty() const { return m_host.dirty(); }

    /* Paints dirty panel subtrees into the retained band surface and
     * adds the changed regions to the screen damage list. */
    void flush(damage_list& damage);

    /* Blits the band's intersection with one compose rect. */
    void compose(stlxgfx_surface_t* back, const damage_list::rect& r);

    bool contains(int32_t x, int32_t y) const {
        if (x < 0 || x >= static_cast<int32_t>(m_width)) {
            return false;
        }

        return (y >= 0 && y < BAR_H) || y >= m_dock_y;
    }

    void pointer_move(int32_t x, int32_t y);
    void pointer_button(int32_t x, int32_t y, uint8_t btn, bool down);

    /* Nanoseconds until the next second rollover, and the tick that
     * applies it. The clock is the desktop's only idle timer. */
    int64_t clock_timeout_ns(uint64_t now_ns) const;
    void clock_tick();

private:
    direct_host m_host;
    direct_host m_dock_host;
    stlxgfx_surface_t* m_band = nullptr;
    stlxgfx_surface_t* m_dock = nullptr;
    uint32_t m_width = 0;
    int32_t m_dock_y = 0;
    ui::label* m_clock = nullptr;
    ui::label* m_net = nullptr;
    uint32_t m_net_tick = 0;
};

#endif
