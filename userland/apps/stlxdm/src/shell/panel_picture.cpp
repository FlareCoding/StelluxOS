#include "panel_picture.hpp"

#include "../material.hpp"

#include <stlxgfx/blend.h>

#include <algorithm>

constexpr uint32_t CORNERS = 4;
constexpr uint32_t BYTES_PER_PIXEL = 4;

static uint32_t* pixel_at(stlxgfx_surface_t* surface, int32_t x, int32_t y) {
    uint8_t* row = surface->pixels + static_cast<size_t>(y) * surface->pitch;
    return reinterpret_cast<uint32_t*>(row) + x;
}

/* The first pixel of a box's corner square, counted from the box's top left: top left, top right, bottom
 * left, then bottom right */
static ui::point corner_origin(uint32_t corner, const ui::rect& box, int32_t radius) {
    int32_t x = (corner & 1) != 0 ? box.w - radius : 0;
    int32_t y = (corner & 2) != 0 ? box.h - radius : 0;

    return { x, y };
}

panel_picture::~panel_picture() {
    drop();
}

bool panel_picture::make(picture_maker* maker, const panel_style& style, int32_t w, int32_t h, int32_t screen_x,
                         int32_t screen_y, const stlxgfx_surface_t* frost) {
    drop();

    uint32_t picture_w = static_cast<uint32_t>(w + 2 * style.shadow_side);
    uint32_t picture_h = static_cast<uint32_t>(style.shadow_top + h + style.shadow_bottom);
    uint32_t pitch = picture_w * BYTES_PER_PIXEL;
    if (maker) {
        m_memory = twinned_picture::make(*maker, picture_w, picture_h);
    }

    if (m_memory) {
        m_surface = stlxgfx_surface_from_buffer(m_memory->pixels(), picture_w, picture_h, pitch, 32, 16, 8, 0);
    } else if (!maker) {
        m_surface = stlxgfx_create_surface(picture_w, picture_h, 32, 16, 8, 0);
    }

    m_background = stlxgfx_create_surface(static_cast<uint32_t>(w), static_cast<uint32_t>(h), 32, 16, 8, 0);
    stlxgfx_surface_t* rim = make_ring_picture(w, h, style.radius, style.rim);
    bool is_drawable = m_surface && m_background && rim;
    if (!is_drawable) {
        stlxgfx_destroy_surface(rim);
        drop();
        return false;
    }

    m_surface->flags = STLXGFX_SURFACE_STRAIGHT_ALPHA;
    m_box = { style.shadow_side, style.shadow_top, w, h };
    m_radius = style.radius;
    paint_drop_shadow(m_surface, m_box, m_radius, style.contact_alpha, style.shadow, style.shadow_layer_count);

    material::surface glass = { { screen_x, screen_y, w, h }, 0, style.rim_fade };
    material::draw(m_background, screen_x, screen_y, frost, glass, glass.bounds);
    stlxgfx_blit_alpha(m_background, 0, 0, rim, 0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    stlxgfx_destroy_surface(rim);

    uint8_t* first = reinterpret_cast<uint8_t*>(pixel_at(m_surface, m_box.x, m_box.y));
    m_content = stlxgfx_surface_from_buffer(first, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                            m_surface->pitch, 32, 16, 8, 0);
    if (!m_content) {
        drop();
        return false;
    }

    m_under_corners.resize(static_cast<size_t>(CORNERS * m_radius * m_radius));
    for (uint32_t corner = 0; corner < CORNERS; corner++) {
        ui::point origin = corner_origin(corner, m_box, m_radius);
        for (int32_t dy = 0; dy < m_radius; dy++) {
            for (int32_t dx = 0; dx < m_radius; dx++) {
                size_t index = (corner * m_radius + dy) * m_radius + dx;
                m_under_corners[index] = *pixel_at(m_content, origin.x + dx, origin.y + dy);
            }
        }
    }

    return true;
}

void panel_picture::drop() {
    stlxgfx_destroy_surface(m_content);
    stlxgfx_destroy_surface(m_background);
    stlxgfx_destroy_surface(m_surface);
    m_content = nullptr;
    m_background = nullptr;
    m_surface = nullptr;

    m_memory.reset();
    m_under_corners.clear();
}

void panel_picture::finish_paint(const std::vector<ui::rect>& painted) {
    for (const ui::rect& r : painted) {
        damage_list::rect inside = damage_list::intersect({ r.x, r.y, r.w, r.h }, { 0, 0, m_box.w, m_box.h });
        bool is_empty = inside.w <= 0 || inside.h <= 0;
        if (m_memory && !is_empty) {
            m_memory->mark_changed({ m_box.x + inside.x, m_box.y + inside.y, inside.w, inside.h });
        }
    }

    ui::rect whole = { 0, 0, m_box.w, m_box.h };
    for (uint32_t corner = 0; corner < CORNERS; corner++) {
        ui::point origin = corner_origin(corner, m_box, m_radius);
        for (int32_t dy = 0; dy < m_radius; dy++) {
            for (int32_t dx = 0; dx < m_radius; dx++) {
                int32_t x = origin.x + dx;
                int32_t y = origin.y + dy;
                bool reached = std::any_of(painted.begin(), painted.end(), [&](const ui::rect& r) {
                    return r.contains({ x, y });
                });

                if (!reached) {
                    continue;
                }

                uint32_t coverage = rounded_rect_coverage(x, y, whole, m_radius);
                uint32_t under = m_under_corners[(corner * m_radius + dy) * m_radius + dx];
                uint32_t* pixel = pixel_at(m_content, x, y);
                *pixel = coverage == 0 ? under : stlxgfx_blend_straight(under, (*pixel & 0x00FFFFFFu) | coverage << 24);
            }
        }
    }
}
