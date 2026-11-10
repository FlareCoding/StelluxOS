#ifndef STLXDM_LAYERS_HPP
#define STLXDM_LAYERS_HPP

#include "damage.hpp"
#include "material.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

class window_import;

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
};

/* One layer as the compose shader reads it, which compose.comp's `layer`
 * must match */
struct layer {
    int32_t  x0, y0;
    int32_t  x1, y1;                /* past the layer's last pixel */
    uint64_t address;
    int32_t  origin_x, origin_y;    /* the output pixel the source's first pixel lies at */
    uint32_t pitch;
    layer_op op;
    uint32_t color;
    uint32_t radius;                /* of the rectangle's rounded bottom corners, all four for glass, 0 when square */
    uint32_t rim_fade;              /* rows a glass rectangle's rim light fades over, 0 for none */
};

static_assert(sizeof(layer) == 56);

/* A picture's first pixel in GPU memory, its pitch, and the output pixel it
 * lies at */
struct layer_picture {
    uint64_t address = 0;
    uint32_t pitch = 0;
    int32_t  x = 0;
    int32_t  y = 0;
};

/* A picture over the part of the output it shows */
struct layer_piece {
    damage_list::rect bounds;
    layer_picture     picture;
};

/* A window as its layers show it. A borderless one has no frame strips,
 * one without a picture the GPU reads shows the content color, and
 * glass with empty bounds shows none. */
struct layer_window {
    static constexpr uint32_t FRAME_STRIPS = 4;

    material::surface    glass;
    damage_list::rect    content;
    uint32_t             radius = 0;
    layer_piece          frame[FRAME_STRIPS];
    bool                 has_picture = false;
    bool                 is_premultiplied = false;  /* `picture` blends over what lies beneath */
    layer_picture        picture;
    const window_import* import = nullptr;          /* the window buffer `picture` lies in */
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
    layer_piece               lower_top;
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

#endif
