/* The painter: clipped, translated drawing over a stlxgfx surface.
 * Widgets draw in local coordinates, the host sets the origin and the
 * base clip, and every operation intersects the clip stack.
 */
#include <stlxui/stlxui.h>

#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>

namespace ui {

/* Faces opened once per pixel size and kept for the process life */
struct font_entry {
    uint32_t px = 0;
    stlxgfx_font* font = nullptr;
};

static std::vector<font_entry> g_fonts;

static stlxgfx_font* font_for(uint32_t px) {
    if (px == 0) {
        px = theme::active().font_size;
    }

    for (auto& e : g_fonts) {
        if (e.px == px) {
            return e.font;
        }
    }

    stlxgfx_font* f = stlxgfx_font_open(STLXGFX_FONT_PATH, px);
    if (f) {
        g_fonts.push_back({ px, f });
    }

    return f;
}

/* Clip intersection in surface coordinates, empty results collapse
 * to a zero rect */
static rect intersect(const rect& a, const rect& b) {
    int32_t x0 = a.x > b.x ? a.x : b.x;
    int32_t y0 = a.y > b.y ? a.y : b.y;
    int32_t x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int32_t y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;

    if (x1 <= x0 || y1 <= y0) {
        return { 0, 0, 0, 0 };
    }

    return { x0, y0, x1 - x0, y1 - y0 };
}

void painter::fill(const rect& r, color c) {
    if (!m_target || m_clips.empty()) {
        return;
    }

    rect surf = { r.x + m_origin.x, r.y + m_origin.y, r.w, r.h };
    rect clipped = intersect(surf, m_clips.back());
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }

    stlxgfx_fill_rect(static_cast<stlxgfx_surface_t*>(m_target),
                      clipped.x, clipped.y,
                      static_cast<uint32_t>(clipped.w),
                      static_cast<uint32_t>(clipped.h), c);
}

void painter::stroke(const rect& r, color c) {
    fill({ r.x, r.y, r.w, 1 }, c);
    fill({ r.x, r.y + r.h - 1, r.w, 1 }, c);
    fill({ r.x, r.y, 1, r.h }, c);
    fill({ r.x + r.w - 1, r.y, 1, r.h }, c);
}

/* Lines clip coarsely by bounding box, which covers every separator
 * and underline the widget set draws */
void painter::line(point a, point b, color c) {
    if (!m_target || m_clips.empty()) {
        return;
    }

    int32_t sx0 = a.x + m_origin.x;
    int32_t sy0 = a.y + m_origin.y;
    int32_t sx1 = b.x + m_origin.x;
    int32_t sy1 = b.y + m_origin.y;

    rect bounds = { sx0 < sx1 ? sx0 : sx1, sy0 < sy1 ? sy0 : sy1,
                    (sx0 < sx1 ? sx1 - sx0 : sx0 - sx1) + 1,
                    (sy0 < sy1 ? sy1 - sy0 : sy0 - sy1) + 1 };
    rect clipped = intersect(bounds, m_clips.back());
    if (clipped.w != bounds.w || clipped.h != bounds.h) {
        return;
    }

    stlxgfx_draw_line(static_cast<stlxgfx_surface_t*>(m_target),
                      sx0, sy0, sx1, sy1, c);
}

void painter::text(point baseline_origin, std::string_view utf8,
                   uint32_t font_size, color c) {
    if (!m_target || m_clips.empty() || utf8.empty()) {
        return;
    }

    stlxgfx_font* f = font_for(font_size);
    if (!f) {
        return;
    }

    /* Glyph blitting clips per pixel to the surface, and the clip
     * stack guards the common whole widget case by bounding box */
    stlxgfx_font_metrics m;
    stlxgfx_font_metrics_get(f, &m);
    int32_t w = stlxgfx_text_width(f, utf8.data(), utf8.size());
    rect bounds = { baseline_origin.x + m_origin.x - 1,
                    baseline_origin.y + m_origin.y - m.ascent,
                    w + 2, m.ascent + m.descent };
    rect clipped = intersect(bounds, m_clips.back());
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }

    stlxgfx_draw_text(static_cast<stlxgfx_surface_t*>(m_target), f,
                      baseline_origin.x + m_origin.x,
                      baseline_origin.y + m_origin.y,
                      utf8.data(), utf8.size(), c);
}

size painter::measure_text(std::string_view utf8,
                           uint32_t font_size) const {
    stlxgfx_font* f = font_for(font_size);
    if (!f) {
        return { 0, 0 };
    }

    stlxgfx_font_metrics m;
    stlxgfx_font_metrics_get(f, &m);

    return { stlxgfx_text_width(f, utf8.data(), utf8.size()),
             m.line_height };
}

int32_t painter::font_ascent(uint32_t font_size) const {
    stlxgfx_font* f = font_for(font_size);
    if (!f) {
        return 0;
    }

    stlxgfx_font_metrics m;
    stlxgfx_font_metrics_get(f, &m);

    return m.ascent;
}

/* Blitting clips coarsely by bounding box, full precision arrives
 * with a consumer that needs partially visible images */
void painter::image(point dst, const void* stlxgfx_surface,
                    int32_t corner_radius) {
    if (!m_target || m_clips.empty() || !stlxgfx_surface) {
        return;
    }

    const stlxgfx_surface_t* src =
        static_cast<const stlxgfx_surface_t*>(stlxgfx_surface);
    rect bounds = { dst.x + m_origin.x, dst.y + m_origin.y,
                    static_cast<int32_t>(src->width),
                    static_cast<int32_t>(src->height) };
    rect clipped = intersect(bounds, m_clips.back());
    if (clipped.w != bounds.w || clipped.h != bounds.h) {
        return;
    }

    if (corner_radius > 0) {
        stlxgfx_blit_rounded_alpha(static_cast<stlxgfx_surface_t*>(m_target),
                                   bounds.x, bounds.y, src, 0, 0,
                                   src->width, src->height,
                                   static_cast<uint32_t>(corner_radius));
        return;
    }

    stlxgfx_blit_alpha(static_cast<stlxgfx_surface_t*>(m_target),
                       bounds.x, bounds.y,
                       const_cast<stlxgfx_surface_t*>(src), 0, 0,
                       src->width, src->height);
}

void painter::circle(point center, int32_t radius, color c) {
    if (!m_target || m_clips.empty() || radius <= 0) {
        return;
    }

    rect bounds = { center.x + m_origin.x - radius,
                    center.y + m_origin.y - radius,
                    2 * radius + 1, 2 * radius + 1 };
    rect clipped = intersect(bounds, m_clips.back());
    if (clipped.w != bounds.w || clipped.h != bounds.h) {
        return;
    }

    stlxgfx_fill_circle(static_cast<stlxgfx_surface_t*>(m_target),
                        center.x + m_origin.x, center.y + m_origin.y,
                        static_cast<uint32_t>(radius), c);
}

void painter::rounded_rect(const rect& r, int32_t radius, color c) {
    if (!m_target || m_clips.empty() || r.w <= 0 || r.h <= 0) {
        return;
    }

    rect surf = { r.x + m_origin.x, r.y + m_origin.y, r.w, r.h };
    rect clipped = intersect(surf, m_clips.back());
    if (clipped.w != surf.w || clipped.h != surf.h) {
        return;
    }

    stlxgfx_fill_rounded_rect(static_cast<stlxgfx_surface_t*>(m_target),
                              surf.x, surf.y,
                              static_cast<uint32_t>(surf.w),
                              static_cast<uint32_t>(surf.h),
                              static_cast<uint32_t>(radius), c);
}

void painter::push_clip(const rect& r) {
    rect surf = { r.x + m_origin.x, r.y + m_origin.y, r.w, r.h };

    if (m_clips.empty()) {
        m_clips.push_back(surf);
        return;
    }

    m_clips.push_back(intersect(surf, m_clips.back()));
}

void painter::pop_clip() {
    if (!m_clips.empty()) {
        m_clips.pop_back();
    }
}

} // namespace ui
