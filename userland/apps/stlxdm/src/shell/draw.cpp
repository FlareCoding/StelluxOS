#include "draw.hpp"

#include <stlxgfx/ctx.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <vector>

/* The radius of the mark's two quarter discs */
constexpr int32_t MARK_RADIUS = 7;

/* Glyph edges reach one pixel either side of their advance */
constexpr int32_t TEXT_SPILL = 1;

struct shell_face {
    std::string   path;
    float         em_px = 0;
    stlxgfx_font* font = nullptr;
};

static std::vector<shell_face> g_faces;

static stlxgfx_surface_t* make_clear_picture(int32_t w, int32_t h) {
    stlxgfx_surface_t* picture = stlxgfx_create_surface(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                                        32, 16, 8, 0);
    if (!picture) {
        return nullptr;
    }

    picture->flags = STLXGFX_SURFACE_STRAIGHT_ALPHA;
    stlxgfx_fill_rect(picture, 0, 0, picture->width, picture->height, 0);

    return picture;
}

stlxgfx_surface_t* make_mark_picture(uint32_t color) {
    stlxgfx_surface_t* picture = make_clear_picture(STLXTHEME_GLYPH_SIZE, STLXTHEME_GLYPH_SIZE);
    if (!picture) {
        return nullptr;
    }

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, picture);
    stlxgfx_ctx_fill_arc_corner(&ctx, 0, 0, MARK_RADIUS, 0, 1, 1, 0, color);
    stlxgfx_ctx_fill_arc_corner(&ctx, STLXTHEME_GLYPH_SIZE, STLXTHEME_GLYPH_SIZE, MARK_RADIUS, 0, -1, -1, 0, color);

    return picture;
}

stlxgfx_font* shell_font(const char* path, float em_px) {
    for (const shell_face& face : g_faces) {
        if (face.path == path && face.em_px == em_px) {
            return face.font;
        }
    }

    stlxgfx_font* font = stlxgfx_font_open(path, em_px);
    if (font) {
        g_faces.push_back({ path, em_px, font });
    }

    return font;
}

uint32_t lit_cells(uint64_t part, uint64_t whole, uint32_t cells) {
    if (whole == 0) {
        return 1;
    }

    return std::max(static_cast<uint32_t>((part * cells + whole - 1) / whole), 1u);
}

text_picture::text_picture(std::string text, const char* face_path, float em_px, uint32_t color)
    : m_text(std::move(text)), m_face_path(face_path), m_em_px(em_px), m_color(color) {
    render();
}

text_picture::~text_picture() {
    stlxgfx_destroy_surface(m_picture);
}

bool text_picture::set_text(std::string text) {
    if (text == m_text) {
        return false;
    }

    m_text = std::move(text);
    render();

    return true;
}

int32_t text_picture::width() const {
    stlxgfx_font* font = shell_font(m_face_path.c_str(), m_em_px);
    return font ? stlxgfx_text_width(font, m_text.data(), m_text.size()) : 0;
}

int32_t text_picture::line_height() const {
    stlxgfx_font* font = shell_font(m_face_path.c_str(), m_em_px);
    if (!font) {
        return 0;
    }

    stlxgfx_font_metrics metrics;
    stlxgfx_font_metrics_get(font, &metrics);

    return metrics.line_height;
}

int32_t text_picture::advance() const {
    stlxgfx_font* font = shell_font(m_face_path.c_str(), m_em_px);
    if (!font) {
        return 0;
    }

    stlxgfx_font_metrics metrics;
    stlxgfx_font_metrics_get(font, &metrics);

    return metrics.advance_mono;
}

void text_picture::draw(ui::painter& p, int32_t x, int32_t top, int32_t box_h) const {
    if (m_picture) {
        p.image({ x - TEXT_SPILL, top + (box_h - line_height()) / 2 }, m_picture);
    }
}

void text_picture::render() {
    stlxgfx_destroy_surface(m_picture);
    m_picture = nullptr;

    stlxgfx_font* font = shell_font(m_face_path.c_str(), m_em_px);
    if (!font || m_text.empty()) {
        return;
    }

    stlxgfx_font_metrics metrics;
    stlxgfx_font_metrics_get(font, &metrics);
    m_picture = make_clear_picture(width() + 2 * TEXT_SPILL, metrics.line_height);
    if (m_picture) {
        stlxgfx_draw_text(m_picture, font, TEXT_SPILL, metrics.ascent, m_text.data(), m_text.size(), m_color);
    }
}
