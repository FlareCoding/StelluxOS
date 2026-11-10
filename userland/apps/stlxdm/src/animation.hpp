#ifndef STLXDM_ANIMATION_HPP
#define STLXDM_ANIMATION_HPP

#include "damage.hpp"
#include "decor.hpp"
#include "layers.hpp"
#include "picture.hpp"

#include <cstdint>
#include <vector>

struct dm_window;

/* What a window's animation shows */
enum class window_transition {
    resize,      /* the window moves between two rects, its frame strips keeping their thickness */
    minimize,    /* the window shrinks into its dock tile, fading out, and stays hidden */
    unminimize,  /* the window grows out of its dock tile, fading in */
};

/* A toplevel shown resampled while it stays out of the scene. Its frame rect, which its shadow paints around, eases
 * from `from` to `to`, and its opacity, 0 to 255, from `from_opacity` to `to_opacity`. */
struct window_animation {
    dm_window*          window = nullptr;
    window_transition   transition = window_transition::resize;
    damage_list::rect   from;
    damage_list::rect   to;
    uint32_t            from_opacity = 0;
    uint32_t            to_opacity = 0;
    uint64_t            start_ns = 0;
    damage_list::rect   rect;  /* where the last step shows the window's frame */
    uint32_t            opacity = 0;
    decor::chrome_state chrome;             /* the look its frame picture is drawn in, kept for the whole animation */
    bool                has_frame = false;  /* whether the frame picture was drawn in `chrome` */
};

/* Sets the animation's rect and opacity for time `now_ns` and returns whether its easing ended */
bool step_animation(window_animation& a, uint64_t now_ns);

/* The frame rect at `w`'s place of its displayed buffer, which `w` must have */
damage_list::rect shown_frame(const dm_window& w);

/* What an animation of `w` paints with its frame at `frame`: the frame and the farthest its shadow reaches past it */
damage_list::rect painted_rect(const dm_window& w, const damage_list::rect& frame);

/* The glass of a decorated window whose frame lies at `frame` */
material::surface frame_glass(const damage_list::rect& frame);

/* The glass under `a`'s frame in the pixels of an output whose top-left pixel lies at (`origin_x`, `origin_y`) on the
 * desktop, empty unless `a` is a resize at full opacity */
material::surface animation_glass(const window_animation& a, int32_t origin_x, int32_t origin_y);

/* Appends to `pieces` the strips of `frame`, unless it is null, and `w`'s displayed buffer, which may differ from the
 * frame in size, resampled into the animation's rect and the shadow around it, in desktop coordinates */
void animation_pieces(const window_animation& a, const dm_window& w, const frame_picture* frame,
                      std::vector<layer_scaled>& pieces);

/* Blends `piece` into `target` inside `clip`, which is in desktop coordinates and must lie inside the target's
 * surface, as compose.comp's `scale` blends it */
void draw_scaled(const decor::canvas& target, const layer_scaled& piece, const damage_list::rect& clip);

/* The largest rect with the proportions of `shape` that fits inside `tile`, centered on it */
damage_list::rect fit_rect(const damage_list::rect& shape, const damage_list::rect& tile);

#endif
