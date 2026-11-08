#ifndef STLXDM_DECOR_HPP
#define STLXDM_DECOR_HPP

#include "damage.hpp"

#include <stlxgfx/surface.h>

#include <cstdint>

struct dm_window;

/* Server-side window chrome. Window x and y remain the content origin
 * everywhere, decorations extend around it, and borderless windows
 * have none. */
namespace decor {

constexpr int32_t TITLE_H = 32;
constexpr int32_t BORDER = 2;
constexpr int32_t CORNER_R = 8;
constexpr int32_t CONTENT_R = CORNER_R - BORDER;  /* content's bottom corners */
constexpr int32_t CLOSE_R = 10;
constexpr int32_t CLOSE_MARGIN = 8;

/* The thin border is a hard target, so resize grips extend this far
 * beyond the visual frame, and this far along it at the corners */
constexpr int32_t RESIZE_SLOP = 4;
constexpr int32_t CORNER_REACH = 16;
constexpr int32_t OUTLINE_T = 2;

/* Pointer zones inside a window's decorated bounds */
enum class zone {
    none,
    content,
    title,
    close,
    resize_l,
    resize_r,
    resize_t,
    resize_b,
    resize_tl,
    resize_tr,
    resize_bl,
    resize_br,
};

/* The surface a compose draws into, and the desktop point its top-left
 * pixel shows. */
struct canvas {
    stlxgfx_surface_t* surface = nullptr;
    int32_t            x = 0;
    int32_t            y = 0;
};

/* Interaction state the chrome renders: the drag glow, the focus
 * palette, and the close control's hover and press shades */
struct chrome_state {
    bool focused = false;
    bool dragging = false;
    bool close_hover = false;
    bool close_pressed = false;
};

/* The edge distances the content's rounded corners read, indexed by
 * squared offset, and the distance of an offset past the table */
struct corner_table {
    const uint16_t* distances = nullptr;
    uint32_t        count = 0;
    int32_t         far_distance = 0;
};

/* Opens the chrome font. Returns 0, or -1 when the face is missing. */
int init();

/* The table `init` fills, which the GPU compositor reads too */
corner_table corner_distances();

bool decorated(const dm_window& w);

/* The decorated rect for an explicit content geometry. */
damage_list::rect frame_rect(const dm_window& w, int32_t x, int32_t y,
                             int32_t cw, int32_t ch);

/* The full on-screen rect including chrome and the drag glow ring,
 * the content rect when borderless. Zero without a displayed buffer. */
damage_list::rect bounds(const dm_window& w);

/* Zone under a desktop point, none when outside the window. Resize
 * zones exist only for resizable decorated windows. */
zone hit(const dm_window& w, int32_t x, int32_t y);

/* Draws the rounded chrome around the content area, clipped to one
 * compose rect so lower chrome never overpaints higher windows. The
 * rect is in desktop coordinates and must lie inside `back`. */
void draw(const canvas& back, const dm_window& w,
          const chrome_state& st, const damage_list::rect& clip);

/* Blits the displayed buffer into place with the frame's rounded
 * bottom corners applied, clipped to one compose rect, which is in
 * desktop coordinates and must lie inside `back`. */
void blit_content(const canvas& back, const dm_window& w,
                  const damage_list::rect& clip);

/* Draws the interactive-resize rubber band `r`, inside `clip` only,
 * both in desktop coordinates, and `clip` must lie inside `back`. */
void draw_outline(const canvas& back, const damage_list::rect& r,
                  const damage_list::rect& clip);

} // namespace decor

#endif
