#ifndef STLXDM_SHELL_PANEL_PICTURE_HPP
#define STLXDM_SHELL_PANEL_PICTURE_HPP

#include "../picture.hpp"
#include "draw.hpp"

#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <memory>
#include <vector>

/* The look `panel_picture::make` draws a panel with: its glass box and the drop shadow around it.
 * `shadow_side`, `shadow_top` and `shadow_bottom` give the room the shadow takes beside, above and below
 * the box. */
struct panel_style {
    int32_t             radius;
    uint32_t            rim;
    int32_t             rim_fade;
    uint32_t            contact_alpha; /* of the one pixel ring around the box */
    const shadow_layer* shadow;
    uint32_t            shadow_layer_count;
    int32_t             shadow_side;
    int32_t             shadow_top;
    int32_t             shadow_bottom;
};

/* The straight alpha picture of a shell panel: its drop shadow, drawn once, and its toolkit tree's opaque
 * pixels in a rounded box. The tree paints into `content()`, its root drawing `background()` (the glass with
 * the rim), and after every paint the caller must pass the painted rects to `round_corners()`, which blends
 * the corners the paint reached over the shadow beneath them. */
class panel_picture {
public:
    panel_picture() = default;
    ~panel_picture();
    panel_picture(const panel_picture&) = delete;
    panel_picture& operator=(const panel_picture&) = delete;

    /* Makes the picture for a `w` by `h` panel whose top-left pixel lies at (`screen_x`, `screen_y`) on the main
     * output, on glass from its `frost`, in `maker`'s memory or the CPU's own when null. False when memory ran out,
     * with no picture left. */
    bool make(picture_maker* maker, const panel_style& style, int32_t w, int32_t h, int32_t screen_x,
              int32_t screen_y, const stlxgfx_surface_t* frost);
    void drop();

    stlxgfx_surface_t* surface() const { return m_surface; }
    stlxgfx_surface_t* content() const { return m_content; }
    const stlxgfx_surface_t* background() const { return m_background; }

    /* The GPU's address of the first pixel, 0 in the CPU's own memory */
    uint64_t address() const { return m_memory ? m_memory->address() : 0; }

    /* The panel's box inside the picture */
    ui::rect box() const { return m_box; }

    /* Rounds the corners inside any of `painted`, the rects of `content()` the tree just painted. A paint must
     * round once, since rounding a corner pixel again blends it over the shadow a second time. */
    void round_corners(const std::vector<ui::rect>& painted);

private:
    std::unique_ptr<picture> m_memory;
    stlxgfx_surface_t*       m_surface = nullptr;
    stlxgfx_surface_t*       m_content = nullptr;
    stlxgfx_surface_t*       m_background = nullptr;
    ui::rect                 m_box;
    int32_t                  m_radius = 0;

    /* The shadow under the four corner squares, each `m_radius` rows of `m_radius` pixels */
    std::vector<uint32_t>    m_under_corners;
};

#endif
