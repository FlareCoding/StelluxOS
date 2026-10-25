#ifndef STLXDM_CURSOR_HPP
#define STLXDM_CURSOR_HPP

#include "damage.hpp"
#include "presenter.hpp"

#include <stlxgfx/surface.h>
#include <stlxwin/proto.h>

#include <cstdint>

/* One pointer shape: the image, its coverage-derived drop shadow, the
 * two as one cursor picture, and the hotspot at the pointer position */
struct cursor_sprite {
    stlxgfx_surface_t* image = nullptr;
    stlxgfx_surface_t* shadow = nullptr;
    stlxgfx_surface_t* picture = nullptr;
    int32_t hot_x = 0;
    int32_t hot_y = 0;
};

/* The pointer's shapes for composed frames and a hardware cursor. The
 * arrow prefers its designed asset and falls back to a procedural one,
 * the other shapes are procedural. */
class cursor {
public:
    int init();
    void shutdown();

    /* Screen bounds of shape s drawn at pointer position (x, y). */
    void bounds(uint32_t shape, int32_t x, int32_t y,
                int32_t* bx, int32_t* by, int32_t* bw, int32_t* bh) const;

    /* Alpha-blits shadow then image onto the target, inside `clip` only,
     * which must lie inside `back`. */
    void draw(stlxgfx_surface_t* back, uint32_t shape,
              int32_t x, int32_t y, const damage_list::rect& clip) const;

    /* Fills `out` with `shape`'s hardware cursor picture, whose pixels
     * stay valid until `shutdown()`. Returns false for a shape without
     * one. */
    bool picture(uint32_t shape, presenter::cursor_picture& out) const;

private:
    cursor_sprite m_arrow;
    cursor_sprite m_ibeam;
    cursor_sprite m_resize_h;
    cursor_sprite m_resize_v;
    cursor_sprite m_resize_nwse;
    cursor_sprite m_resize_nesw;

    const cursor_sprite* sprite_for(uint32_t shape) const;
};

#endif
