#include "draw.hpp"

#include <stlxgfx/ctx.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cmath>
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

stlxgfx_surface_t* make_ring_picture(int32_t w, int32_t h, int32_t radius, uint32_t color) {
    stlxgfx_surface_t* picture = make_clear_picture(w, h);
    if (!picture) {
        return nullptr;
    }

    uint32_t r = static_cast<uint32_t>(radius);
    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, picture);

    stlxgfx_ctx_fill_arc_corner(&ctx, 0, 0, r, r - 1, 1, 1, 0, color);
    stlxgfx_ctx_fill_arc_corner(&ctx, w, 0, r, r - 1, -1, 1, 0, color);
    stlxgfx_ctx_fill_arc_corner(&ctx, 0, h, r, r - 1, 1, -1, 0, color);
    stlxgfx_ctx_fill_arc_corner(&ctx, w, h, r, r - 1, -1, -1, 0, color);

    uint32_t straight_w = static_cast<uint32_t>(w - 2 * radius);
    uint32_t straight_h = static_cast<uint32_t>(h - 2 * radius);
    stlxgfx_fill_rect_blend(picture, radius, 0, straight_w, 1, color);
    stlxgfx_fill_rect_blend(picture, radius, h - 1, straight_w, 1, color);
    stlxgfx_fill_rect_blend(picture, 0, radius, 1, straight_h, color);
    stlxgfx_fill_rect_blend(picture, w - 1, radius, 1, straight_h, color);

    return picture;
}

/* Signed distance from the center of pixel (`x`, `y`) to the edge of a rect
 * with corners of `radius`, negative inside */
static float rounded_rect_distance(int32_t x, int32_t y, const ui::rect& box, int32_t radius) {
    float half_w = static_cast<float>(box.w) * 0.5f;
    float half_h = static_cast<float>(box.h) * 0.5f;
    float past_x = std::fabs(static_cast<float>(x) + 0.5f - (static_cast<float>(box.x) + half_w)) - (half_w - radius);
    float past_y = std::fabs(static_cast<float>(y) + 0.5f - (static_cast<float>(box.y) + half_h)) - (half_h - radius);

    float outside = std::hypot(std::max(past_x, 0.0f), std::max(past_y, 0.0f));
    float inside = std::min(std::max(past_x, past_y), 0.0f);

    return outside + inside - static_cast<float>(radius);
}

/* The share of a gaussian of `sigma` around `center` that lies between `start` and `end` */
static float blurred_span(float center, float start, float end, float sigma) {
    float scale = 1.0f / (sigma * std::sqrt(2.0f));
    return 0.5f * (std::erf((end - center) * scale) - std::erf((start - center) * scale));
}

/* Whether the pixel lies in `box` clear of its corner squares, where the box hides its shadow */
static bool is_covered_by_box(int32_t x, int32_t y, const ui::rect& box, int32_t radius) {
    bool in_corner_column = x < box.x + radius || x >= box.x + box.w - radius;
    bool in_corner_row = y < box.y + radius || y >= box.y + box.h - radius;

    return box.contains({ x, y }) && !(in_corner_column && in_corner_row);
}

uint32_t rounded_rect_coverage(int32_t x, int32_t y, const ui::rect& box, int32_t radius) {
    float coverage = std::clamp(0.5f - rounded_rect_distance(x, y, box, radius), 0.0f, 1.0f);
    return static_cast<uint32_t>(coverage * 255.0f + 0.5f);
}

void paint_drop_shadow(stlxgfx_surface_t* picture, const ui::rect& box, int32_t radius, uint32_t ring_alpha,
                       const shadow_layer* layers, uint32_t layer_count) {
    int32_t width = static_cast<int32_t>(picture->width);
    int32_t height = static_cast<int32_t>(picture->height);
    std::vector<float> columns(static_cast<size_t>(width) * layer_count);
    std::vector<float> rows(static_cast<size_t>(height) * layer_count);

    /* A blurred box is the product of its blurred spans across and down,
     * with CSS taking the blur radius as twice the deviation */
    for (uint32_t i = 0; i < layer_count; i++) {
        float sigma = static_cast<float>(layers[i].blur) * 0.5f;
        float top = static_cast<float>(box.y + layers[i].offset_y);
        for (int32_t x = 0; x < width; x++) {
            columns[i * width + x] = blurred_span(x + 0.5f, box.x, box.x + box.w, sigma);
        }

        for (int32_t y = 0; y < height; y++) {
            rows[i * height + y] = blurred_span(y + 0.5f, top, top + box.h, sigma);
        }
    }

    ui::rect ring = { box.x - 1, box.y - 1, box.w + 2, box.h + 2 };
    for (int32_t y = 0; y < height; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(picture->pixels + static_cast<size_t>(y) * picture->pitch);
        for (int32_t x = 0; x < width; x++) {
            if (is_covered_by_box(x, y, box, radius)) {
                row[x] = 0;
                continue;
            }

            float light = 1.0f;
            for (uint32_t i = 0; i < layer_count; i++) {
                light *= 1.0f - layers[i].alpha / 255.0f * columns[i * width + x] * rows[i * height + y];
            }

            if (ring.contains({ x, y })) {
                int32_t around = static_cast<int32_t>(rounded_rect_coverage(x, y, ring, radius + 1));
                int32_t within = static_cast<int32_t>(rounded_rect_coverage(x, y, box, radius));
                light *= 1.0f - static_cast<float>(ring_alpha * std::max(around - within, 0)) / (255.0f * 255.0f);
            }

            uint32_t alpha = static_cast<uint32_t>((1.0f - light) * 255.0f + 0.5f);
            row[x] = (alpha << 24) | (STLXTHEME_INK & 0x00FFFFFFu);
        }
    }
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

bool text_picture::set_color(uint32_t color) {
    if (color == m_color) {
        return false;
    }

    m_color = color;
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

mono_label::mono_label(std::string text, float em_px, uint32_t color)
    : m_text(std::move(text), STLXTHEME_MONO_FONT_PATH, em_px, color) {}

void mono_label::set_text(std::string text) {
    if (m_text.set_text(std::move(text))) {
        invalidate_layout();
    }
}

ui::size mono_label::measure(ui::size) {
    return { m_text.width(), m_text.line_height() };
}

void mono_label::paint(ui::painter& p) {
    m_text.draw(p, 0, 0, m_frame.h);
}
