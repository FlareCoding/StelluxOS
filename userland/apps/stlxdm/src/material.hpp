#ifndef STLXDM_MATERIAL_HPP
#define STLXDM_MATERIAL_HPP

#include "damage.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/surface.h>

#include <cstdint>

/* Glass: an output's frost, the wallpaper blurred and darkened, under the
 * smoke tint, a grey veil graded from the top edge, a grain and a rim
 * light, drawn from integer tables for the configured level and tint so
 * every drawing gives the same pixels */
namespace material {

/* compose.comp repeats these two sizes and the layout of `tables` */
constexpr int32_t  GRAIN_TILE = 160;
constexpr uint32_t VEIL_STEPS = 256;

/* Rows over which the rim light of a window and of the dock fades out
 * below their top edge */
constexpr int32_t WINDOW_RIM_FADE = 72;
constexpr int32_t DOCK_RIM_FADE = 36;

/* A glass surface: a rounded rect in an output's pixels. Its veil and rim
 * light run down from its top edge, and its grain repeats from its
 * top-left pixel. */
struct surface {
    damage_list::rect bounds;
    int32_t           radius = 0;
    int32_t           rim_fade = 0;   /* 0 for no rim light */
};

/* The configured glass as integer tables. A veil entry, indexed by the
 * veil's opacity, holds the 16.16 scale and the blue, green and red
 * offsets that veil and tint a frost channel, and the grain holds a byte
 * a pixel, low byte first. */
struct tables {
    uint32_t rim_alpha;
    uint32_t light;
    uint32_t veil_top;
    uint32_t veil_bottom;
    uint32_t veil[VEIL_STEPS][4];
    uint32_t grain[GRAIN_TILE * GRAIN_TILE / 4];
};

/* Fills the tables for `level` and `tint`. Must run before any glass is
 * drawn. */
void configure(stlxconf_glass_level_t level, stlxconf_glass_tint_t tint);

const tables& current_tables();

/* Counts the `configure` calls, so a copy of the tables can tell when
 * it went stale */
uint32_t tables_generation();

/* The frost of `wallpaper` for `tint`, a surface of the wallpaper's
 * size, or null when memory runs out. `wallpaper` must be the output's
 * wallpaper or flat color at the output's size, without the panels. */
stlxgfx_surface_t* build_frost(const stlxgfx_surface_t* wallpaper, stlxconf_glass_tint_t tint);

/* Draws `glass` from the output's `frost` inside `clip`, which must lie
 * in `target`, whose first pixel shows the output pixel (`x`, `y`). Only
 * the part of `clip` that `frost` covers is drawn. */
void draw(stlxgfx_surface_t* target, int32_t x, int32_t y, const stlxgfx_surface_t* frost, const surface& glass,
          const damage_list::rect& clip);

} // namespace material

#endif
