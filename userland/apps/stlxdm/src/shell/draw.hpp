#ifndef STLXDM_SHELL_DRAW_HPP
#define STLXDM_SHELL_DRAW_HPP

#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <string>

/* One blurred layer of an ink drop shadow, with CSS box-shadow's meaning of offset and blur */
struct shadow_layer {
    int32_t  offset_y;
    int32_t  blur;
    uint32_t alpha; /* of the ink, 255 opaque */
};

/* The Stellux mark, `size` pixels square, as /etc/res/icons/stellux-mark-<size>.png
 * holds it. Null when that file cannot be read, cannot be decoded, or memory ran out. */
stlxgfx_surface_t* make_mark_picture(int32_t size);

/* A one pixel ring along the edge of a `w` by `h` rect with corners of
 * `radius`, in `color` on transparent pixels. `w` and `h` must be at least
 * twice `radius`. Null when memory ran out. */
stlxgfx_surface_t* make_ring_picture(int32_t w, int32_t h, int32_t radius, uint32_t color);

/* A magnifier glass, a ring with a handle toward the bottom right, `size` pixels square in `color` on transparent
 * pixels. Null when memory ran out. */
stlxgfx_surface_t* make_search_glyph(int32_t size, uint32_t color);

/* How much of the pixel at (`x`, `y`) a rect with corners of `radius` covers, 0 to 255 */
uint32_t rounded_rect_coverage(int32_t x, int32_t y, const ui::rect& box, int32_t radius);

/* Paints into `picture` the ink drop shadow of `box`, whose corners have `radius`: a one pixel contact ring
 * at `ring_alpha` around it and the blurred `layers` beneath. The box clear of its corner squares comes out
 * transparent, for the caller to cover. */
void paint_drop_shadow(stlxgfx_surface_t* picture, const ui::rect& box, int32_t radius, uint32_t ring_alpha,
                       const shadow_layer* layers, uint32_t layer_count);

/* The face at `path` and `em_px`, kept for the process once an open
 * succeeds, or null when it cannot be opened */
stlxgfx_font* shell_font(const char* path, float em_px);

/* The cells of a meter of `cells` lit for `part` of `whole`, rounded up and never fewer than one */
uint32_t lit_cells(uint64_t part, uint64_t whole, uint32_t cells);

/* One line of text drawn into a picture once per change. It gives the shell Plex
 * Mono, which the toolkit's labels lack, and text measured outside a paint. */
class text_picture {
public:
    text_picture(std::string text, const char* face_path, float em_px, uint32_t color);
    ~text_picture();
    text_picture(const text_picture&) = delete;
    text_picture& operator=(const text_picture&) = delete;

    /* Each returns whether the value it sets changed */
    bool set_text(std::string text);
    bool set_color(uint32_t color);

    int32_t width() const;
    int32_t line_height() const;
    int32_t advance() const;

    /* Draws with its pen at `x`, on a line centered in the `box_h` rows below `top` */
    void draw(ui::painter& p, int32_t x, int32_t top, int32_t box_h) const;

private:
    void render();

    std::string        m_text;
    std::string        m_face_path;
    float              m_em_px = 0;
    uint32_t           m_color = 0;
    stlxgfx_surface_t* m_picture = nullptr;
};

/* One line of Plex Mono at the left of its frame */
class mono_label : public ui::widget {
public:
    mono_label(std::string text, float em_px, uint32_t color);

    void set_text(std::string text);

    ui::size measure(ui::size avail) override;
    void paint(ui::painter& p) override;

private:
    text_picture m_text;
};

#endif
