/* The painter: clipped, translated drawing over a stlxgfx surface.
 * Widgets draw in local coordinates, the host sets the origin and the
 * base clip, and every operation intersects the clip stack.
 */
#include <stlxui/stlxui.h>

#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>
#include <stlxgfx/surface.h>

namespace ui {

/* Faces opened once per weight and pixel size, kept for the process */
struct font_entry {
    uint32_t px = 0;
    weight w = weight::regular;
    stlxgfx_font* font = nullptr;
};

static std::vector<font_entry> g_fonts;

static const char* face_path(weight w) {
    const theme& t = theme::active();

    switch (w) {
    case weight::medium:   return t.font_medium;
    case weight::semibold: return t.font_semibold;
    default:               return t.font_regular;
    }
}

static stlxgfx_font* font_for(uint32_t px, weight w) {
    if (px == 0) {
        px = theme::active().font_size;
    }

    for (auto& e : g_fonts) {
        if (e.px == px && e.w == w) {
            return e.font;
        }
    }

    /* A missing weight falls back to the regular face rather than
     * losing the text entirely */
    stlxgfx_font* f = stlxgfx_font_open(face_path(w), px);
    if (!f && w != weight::regular) {
        f = stlxgfx_font_open(face_path(weight::regular), px);
    }
    if (f) {
        g_fonts.push_back({ px, w, f });
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

/* A writable view over the clip region, so partially visible shapes
 * render their visible part instead of bailing or overflowing. The
 * caller draws in coordinates shifted by the view's origin. */
static stlxgfx_surface_t* clip_view(void* target, const rect& clipped) {
    stlxgfx_surface_t* s = static_cast<stlxgfx_surface_t*>(target);

    return stlxgfx_surface_from_buffer(
        s->pixels + static_cast<uint32_t>(clipped.y) * s->pitch
            + static_cast<uint32_t>(clipped.x) * 4,
        static_cast<uint32_t>(clipped.w), static_cast<uint32_t>(clipped.h),
        s->pitch, 32, 16, 8, 0);
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

    /* A translucent color composites over what is already painted */
    if ((c >> 24) == 0xFF) {
        stlxgfx_fill_rect(static_cast<stlxgfx_surface_t*>(m_target),
                          clipped.x, clipped.y,
                          static_cast<uint32_t>(clipped.w),
                          static_cast<uint32_t>(clipped.h), c);
    } else {
        stlxgfx_fill_rect_blend(static_cast<stlxgfx_surface_t*>(m_target),
                                clipped.x, clipped.y,
                                static_cast<uint32_t>(clipped.w),
                                static_cast<uint32_t>(clipped.h), c);
    }
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
                   uint32_t font_size, color c, weight w) {
    if (!m_target || m_clips.empty() || utf8.empty()) {
        return;
    }

    stlxgfx_font* f = font_for(font_size, w);
    if (!f) {
        return;
    }

    stlxgfx_font_metrics m;
    stlxgfx_font_metrics_get(f, &m);
    int32_t tw = stlxgfx_text_width(f, utf8.data(), utf8.size());
    rect bounds = { baseline_origin.x + m_origin.x - 1,
                    baseline_origin.y + m_origin.y - m.ascent,
                    tw + 2, m.ascent + m.descent };
    rect clipped = intersect(bounds, m_clips.back());
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }

    /* Drawing through a view over the clip makes overlong text end
     * at its widget instead of spilling across neighbors */
    stlxgfx_surface_t* view = clip_view(m_target, clipped);
    if (!view) {
        return;
    }

    stlxgfx_draw_text(view, f,
                      baseline_origin.x + m_origin.x - clipped.x,
                      baseline_origin.y + m_origin.y - clipped.y,
                      utf8.data(), utf8.size(), c);
    stlxgfx_destroy_surface(view);
}

size painter::measure_text(std::string_view utf8,
                           uint32_t font_size, weight w) const {
    stlxgfx_font* f = font_for(font_size, w);
    if (!f) {
        return { 0, 0 };
    }

    stlxgfx_font_metrics m;
    stlxgfx_font_metrics_get(f, &m);

    return { stlxgfx_text_width(f, utf8.data(), utf8.size()),
             m.line_height };
}

int32_t painter::font_ascent(uint32_t font_size, weight w) const {
    stlxgfx_font* f = font_for(font_size, w);
    if (!f) {
        return 0;
    }

    stlxgfx_font_metrics m;
    stlxgfx_font_metrics_get(f, &m);

    return m.ascent;
}

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
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }

    stlxgfx_surface_t* view = clip_view(m_target, clipped);
    if (!view) {
        return;
    }

    if (corner_radius > 0) {
        stlxgfx_blit_rounded_alpha(view,
                                   bounds.x - clipped.x,
                                   bounds.y - clipped.y, src, 0, 0,
                                   src->width, src->height,
                                   static_cast<uint32_t>(corner_radius));
    } else {
        stlxgfx_blit_alpha(view, bounds.x - clipped.x,
                           bounds.y - clipped.y,
                           const_cast<stlxgfx_surface_t*>(src), 0, 0,
                           src->width, src->height);
    }

    stlxgfx_destroy_surface(view);
}

void painter::circle(point center, int32_t radius, color c) {
    if (!m_target || m_clips.empty() || radius <= 0) {
        return;
    }

    rect bounds = { center.x + m_origin.x - radius,
                    center.y + m_origin.y - radius,
                    2 * radius + 1, 2 * radius + 1 };
    rect clipped = intersect(bounds, m_clips.back());
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }

    stlxgfx_surface_t* view = clip_view(m_target, clipped);
    if (!view) {
        return;
    }

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, view);
    stlxgfx_ctx_fill_circle(&ctx, center.x + m_origin.x - clipped.x,
                            center.y + m_origin.y - clipped.y,
                            static_cast<uint32_t>(radius), c);
    stlxgfx_destroy_surface(view);
}

void painter::rounded_rect(const rect& r, int32_t radius, color c) {
    if (!m_target || m_clips.empty() || r.w <= 0 || r.h <= 0) {
        return;
    }

    rect surf = { r.x + m_origin.x, r.y + m_origin.y, r.w, r.h };
    rect clipped = intersect(surf, m_clips.back());
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }

    /* The view keeps rounded fills correct under partial damage, a
     * repaint of one child re-fills exactly its slice of the panel.
     * The ctx path anti-aliases the corners and honors alpha. */
    stlxgfx_surface_t* view = clip_view(m_target, clipped);
    if (!view) {
        return;
    }

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, view);
    stlxgfx_ctx_fill_rounded_rect(&ctx, surf.x - clipped.x,
                                  surf.y - clipped.y,
                                  static_cast<uint32_t>(surf.w),
                                  static_cast<uint32_t>(surf.h),
                                  static_cast<uint32_t>(radius), c);
    stlxgfx_destroy_surface(view);
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
