#ifndef STLXDM_SHELL_DRAW_HPP
#define STLXDM_SHELL_DRAW_HPP

#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <string>

/* The Stellux mark, a quarter disc at its top left and one at its bottom
 * right, in `color` on a transparent `STLXTHEME_GLYPH_SIZE` square. Null
 * when memory ran out. */
stlxgfx_surface_t* make_mark_picture(uint32_t color);

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

    /* Returns whether the text changed */
    bool set_text(std::string text);

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

#endif
