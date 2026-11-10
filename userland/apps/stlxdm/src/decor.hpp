#ifndef STLXDM_DECOR_HPP
#define STLXDM_DECOR_HPP

#include "damage.hpp"

#include <stlxgfx/surface.h>
#include <stlxtheme/theme.h>

#include <cstdint>

struct dm_window;

/* Server-side window chrome. Window x and y remain the content origin
 * everywhere, decorations extend around it, and borderless windows
 * have none. */
namespace decor {

constexpr int32_t TITLE_H = STLXTHEME_TITLE_BAR_HEIGHT;
constexpr int32_t BORDER = 1;
constexpr int32_t CORNER_R = STLXTHEME_WINDOW_RADIUS;
constexpr int32_t CONTENT_R = CORNER_R - BORDER;  /* content's bottom corners */

/* The color a window's content area shows where no picture of its own
 * covers it */
constexpr uint32_t CONTENT_BG = STLXTHEME_GLASS_TINT_WARM;

/* The thin border is a hard target, so resize grips extend this far
 * beyond the visual frame, and this far along it at the corners */
constexpr int32_t RESIZE_SLOP = 4;
constexpr int32_t CORNER_REACH = 16;
constexpr int32_t OUTLINE_T = 2;

/* The window controls sit in a row at the title bar's right end */
constexpr int32_t CONTROL_COUNT = 3;
constexpr int32_t CONTROL_SIZE = STLXTHEME_WINDOW_CONTROL_SIZE;
constexpr int32_t CONTROL_GAP = STLXTHEME_WINDOW_CONTROL_GAP;
constexpr int32_t CONTROL_INSET = 8;  /* from the close control to the frame's right edge */

/* The minimum content size of an interactive resize, whose width leaves
 * the controls their inset on both sides */
constexpr uint32_t MIN_CONTENT_W =
    2 * CONTROL_INSET + CONTROL_COUNT * CONTROL_SIZE + (CONTROL_COUNT - 1) * CONTROL_GAP - 2 * BORDER;
constexpr uint32_t MIN_CONTENT_H = 48;

/* Pointer zones inside a window's decorated bounds */
enum class zone {
    none,
    content,
    title,
    minimize,
    maximize,
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

/* Interaction state the chrome renders: the focus palette and rule, the
 * window control under the pointer and the one a press holds */
struct chrome_state {
    bool focused = false;
    zone hovered = zone::none;
    zone pressed = zone::none;

    bool operator==(const chrome_state&) const = default;
};

/* The edge distances the content's rounded corners read, indexed by
 * squared offset, and the distance of an offset past the table */
struct corner_table {
    const uint16_t* distances = nullptr;
    uint32_t        count = 0;
    int32_t         far_distance = 0;
};

/* Opens the chrome font and prepares the tables and shadows the other
 * calls read. Returns 0, or -1 when the face is missing. */
int init();

/* The table `init` fills, which the GPU compositor reads too */
corner_table corner_distances();

/* Signed distance from the center of the pixel (`x`, `y`) to the edge
 * of `bounds` rounded by `radius`, in eighth pixels and negative inside,
 * read from the table `init` fills, so `radius` must be at most
 * `CORNER_R`. Far outside it saturates. */
int32_t rounded_rect_distance(const damage_list::rect& bounds, int32_t radius, int32_t x, int32_t y);

/* The coverage, 0 to 255, of a pixel whose center lies `distance`
 * eighth pixels outside an edge, negative inside */
uint32_t distance_coverage(int32_t distance);

/* Source-over of `color` at `alpha`, 0 to 255, onto the opaque `below`,
 * giving an opaque pixel */
uint32_t blend_over(uint32_t below, uint32_t color, uint32_t alpha);

bool decorated(const dm_window& w);

/* The decorated rect for an explicit content geometry. */
damage_list::rect frame_rect(const dm_window& w, int32_t x, int32_t y,
                             int32_t cw, int32_t ch);

/* The full on-screen rect including chrome and the shadow the window
 * casts when focused or not as `is_focused` says, the content rect
 * when borderless. Zero without a displayed buffer. */
damage_list::rect bounds(const dm_window& w, bool is_focused);

/* The rect a window with an explicit content geometry paints focused or
 * not, which damage for it must cover. */
damage_list::rect damage_rect(const dm_window& w, int32_t x, int32_t y, int32_t cw, int32_t ch);

constexpr uint32_t FRAME_STRIPS = 4;

/* `bounds(w, is_focused)` of a decorated window less the content above its
 * last CONTENT_R rows, as the top with the title bar, the left and right
 * sides and the bottom, each in desktop coordinates and empty where
 * nothing lies. All zero when borderless or without a displayed buffer. */
void frame_strips(const dm_window& w, bool is_focused, damage_list::rect (&strips)[FRAME_STRIPS]);

/* Zone under a desktop point, none when outside the window. Resize
 * zones exist only for resizable decorated windows. */
zone hit(const dm_window& w, int32_t x, int32_t y);

/* Whether `z` is one of the window controls. */
bool is_control(zone z);

/* The rect around the window controls `w` shows, in desktop coordinates,
 * empty when it shows none. A window narrower than `MIN_CONTENT_W` shows
 * only the controls that fit with the inset on both sides. */
damage_list::rect controls_rect(const dm_window& w);

/* Draws the window's shadow and rounded chrome around the content area
 * into the straight alpha `back` inside `clip`, which is in desktop
 * coordinates and must lie inside `back`, and leaves the frame's inside
 * clear for what is composed beneath the window to show. The shadow
 * replaces the pixels around the frame's body instead of blending over
 * them, so `back` must be transparent there. */
void draw(const canvas& back, const dm_window& w,
          const chrome_state& st, const damage_list::rect& clip);

/* Draws the displayed buffer into `back` within one compose rect, which
 * is in desktop coordinates and must lie inside `back`: copied, or
 * blended over what `back` holds when premultiplied, with the frame's
 * rounded bottom corners applied. A buffer without a mapping shows
 * `CONTENT_BG` instead. */
void blit_content(const canvas& back, const dm_window& w,
                  const damage_list::rect& clip);

/* Draws the interactive-resize rubber band `r`, inside `clip` only,
 * both in desktop coordinates, and `clip` must lie inside `back`. */
void draw_outline(const canvas& back, const damage_list::rect& r,
                  const damage_list::rect& clip);

} // namespace decor

#endif
