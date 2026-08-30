#ifndef STLXDM_CURSOR_HPP
#define STLXDM_CURSOR_HPP

#include <stlxgfx/surface.h>
#include <stlxwin/proto.h>

#include <cstdint>

/* One pointer shape: the image, a coverage-derived drop shadow, and
 * the hotspot pixel that sits exactly at the pointer position */
struct cursor_sprite {
    stlxgfx_surface_t* image = nullptr;
    stlxgfx_surface_t* shadow = nullptr;
    int32_t hot_x = 0;
    int32_t hot_y = 0;
};

/* The software pointer, topmost layer of every composed rect. The
 * arrow prefers its designed asset and falls back to a procedural
 * shape, other shapes are procedural. */
class cursor {
public:
    int init();
    void shutdown();

    /* Screen bounds of shape s drawn at pointer position (x, y). */
    void bounds(uint32_t shape, int32_t x, int32_t y,
                int32_t* bx, int32_t* by, int32_t* bw, int32_t* bh) const;

    /* Alpha-blits shadow then image onto the target. */
    void draw(stlxgfx_surface_t* back, uint32_t shape,
              int32_t x, int32_t y) const;

private:
    cursor_sprite m_arrow;
    cursor_sprite m_ibeam;
    cursor_sprite m_resize_h;
    cursor_sprite m_resize_v;

    const cursor_sprite* sprite_for(uint32_t shape) const;
};

#endif
