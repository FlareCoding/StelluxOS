#ifndef STLXDM_LAYERS_HPP
#define STLXDM_LAYERS_HPP

#include "damage.hpp"
#include "material.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

class twinned_picture;
class window_import;
class window_twin;

/* Per damaged rectangle of an output, the layers shaders/compose.comp walks
 * bottom to top for each pixel, every rectangle in the output's pixels */

enum class layer_op : uint32_t {
    copy,                 /* the source, its bottom corner squares blended by coverage when rounded */
    blend,                /* the source's straight alpha pixels over what lies beneath */
    fill,                 /* one color, leaving what lies beneath in the bottom corner squares when rounded */
    dim,                  /* what lies beneath darkened by one color at its alpha */
    spin,                 /* 1 added to the pixel without end, which runs a test's compose past its deadline */
    glass,                /* glass from the frost source, its corners blended by coverage when rounded */
    blend_premultiplied,  /* the source's premultiplied alpha pixels over what lies beneath, rounded as `copy` */
    scale,                /* a picture region resampled into the rectangle at an opacity, as `layer_scaled` says */
};

/* One layer as the compose shader reads it, which compose.comp's `layer`
 * must match */
struct layer {
    int32_t  x0, y0;
    int32_t  x1, y1;                /* past the layer's last pixel */
    uint64_t address;
    int32_t  origin_x, origin_y;    /* the output pixel the source's first pixel lies at, or a `scale` source's size */
    uint32_t pitch;
    layer_op op;
    uint32_t color;                 /* a fill's or dim's color, `blend`'s opacity out of 255, or `scale` bits */
    uint32_t radius;                /* of the rectangle's rounded bottom corners, all four for glass, 0 when square */
    uint32_t rim_fade;              /* rows a glass rectangle's rim light fades over, 0 for none */
};

static_assert(sizeof(layer) == 56);

/* A `scale` layer holds its source's size in `origin_x` and `origin_y`, its opacity in `color`'s alpha byte, and in
 * `color` bits for a window's content, whose alpha means nothing unless its pixels are premultiplied */
constexpr uint32_t SCALE_CONTENT = 1u;
constexpr uint32_t SCALE_PREMULTIPLIED = 2u;

/* A picture's first pixel in GPU memory, its pitch, the output pixel it
 * lies at, and the twinned picture `address` lies in, if any */
struct layer_picture {
    uint64_t         address = 0;
    uint32_t         pitch = 0;
    int32_t          x = 0;
    int32_t          y = 0;
    twinned_picture* twinned = nullptr;
};

/* A picture over the part of the output it shows, and the opacity a blend
 * shows it at */
struct layer_piece {
    damage_list::rect bounds;
    layer_picture     picture;
    uint32_t          opacity = 255;
};

/* A region of a picture: its first pixel as the CPU reads it, null without
 * a mapping, and as the GPU reads it, 0 where the GPU cannot reach it, and
 * the twinned picture `address` lies in, if any */
struct picture_region {
    const uint8_t*   pixels = nullptr;
    uint64_t         address = 0;
    uint32_t         pitch = 0;
    int32_t          width = 0;
    int32_t          height = 0;
    twinned_picture* twinned = nullptr;
};

/* A picture region resampled bilinearly into `bounds` at `opacity` from 0 to 255. Content rounds its bottom corners
 * by `radius` and is opaque unless premultiplied, and any other picture blends by its straight alpha. */
struct layer_scaled {
    damage_list::rect bounds;
    picture_region    source;
    uint32_t          opacity = 0;
    bool              is_content = false;
    bool              is_premultiplied = false;
    uint32_t          radius = 0;
};

/* A window as its layers show it: no frame strips when borderless, the content color without a picture the GPU
 * reads, no glass when its bounds are empty, and while it animates `scaled` pieces in place of its frame strips and
 * content. An animating window covers no rect. With a twin, which needs `import`, `picture` and the scaled content lie
 * in the twin, which the compose first brings up to date from the window buffer. */
struct layer_window {
    static constexpr uint32_t FRAME_STRIPS = 4;
    static constexpr uint32_t SCALED_MAX = FRAME_STRIPS * 3 + 1;  /* up to three pieces per strip, and the content */

    material::surface         glass;
    damage_list::rect         content;
    uint32_t                  radius = 0;
    layer_piece               frame[FRAME_STRIPS];
    bool                      has_picture = false;
    bool                      is_premultiplied = false;  /* `picture` blends over what lies beneath */
    layer_picture             picture;
    const window_import*      import = nullptr;          /* the window buffer */
    window_twin*              twin = nullptr;
    uint64_t                  picture_offset = 0;        /* from the window buffer's first byte to its picture's */
    std::vector<layer_scaled> scaled;
};

/* What one output shows, bottom to top, the dim only while the power
 * overlay is active, and each top picture only over the bounds drawn
 * into it */
struct layer_scene {
    int32_t                   width = 0;
    int32_t                   height = 0;
    layer_picture             backdrop;
    layer_picture             frost;          /* address 0 without one, and then no glass shows */
    std::vector<layer_window> windows;
    uint32_t                  content_color = 0;
    layer_piece               popover;        /* the shell's popover, a closing one included */
    layer_piece               lower_top;
    std::vector<layer_scaled> drag;           /* a drag's shadow and picture */
    layer_piece               drag_chip;
    bool                      is_dimmed = false;
    uint32_t                  dim_color = 0;
    layer_piece               upper_top;
};

/* The most layers one list of a scene with `windows` windows holds */
uint32_t max_layers(size_t windows);

/* Appends `r`'s layers to `list` and returns their count, leaving out
 * those outside `r` and those beneath the topmost window covering `r`:
 * by opaque content, whose window's glass and frame go too, or on an output
 * with frost by the glass under premultiplied content */
uint32_t append_layers(const layer_scene& scene, const damage_list::rect& r, std::vector<layer>& list);

/* Appends to `pictures` the twinned picture of each picture `scene` names,
 * some more than once, whose twins a compose of the scene reads */
void append_twinned(const layer_scene& scene, std::vector<twinned_picture*>& pictures);

#endif
