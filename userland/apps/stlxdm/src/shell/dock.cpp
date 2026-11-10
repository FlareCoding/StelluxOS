#include "dock.hpp"

#include "../material.hpp"
#include "panel_picture.hpp"

#include <stlxgfx/image.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstring>

constexpr int32_t TILE_SIZE = STLXTHEME_DOCK_TILE_SIZE;
constexpr int32_t ICON_SIZE = STLXTHEME_DOCK_ICON_SIZE;
constexpr int32_t DOCK_PADDING = (STLXTHEME_DOCK_HEIGHT - TILE_SIZE) / 2;
constexpr int32_t TILE_GAP = 4;
constexpr int32_t DOCK_BOTTOM_MARGIN = 16;

constexpr int32_t TICK_HEIGHT = 2;
constexpr int32_t TICK_RADIUS = 1;
constexpr int32_t TICK_BOTTOM_INSET = 1;
constexpr int32_t TICK_RUNNING_WIDTH = 8;
constexpr int32_t TICK_FOCUSED_WIDTH = 16;

constexpr int32_t DOCK_SHADOW_SIDE = 24;
constexpr int32_t DOCK_SHADOW_TOP = 16;
constexpr uint32_t DOCK_SHADOW_CONTACT_ALPHA = 191; /* ink at .75 */

/* The dock's drop shadow apart from its contact ring: two blurred layers of ink at .24 and .44 */
static const shadow_layer DOCK_SHADOW[] = {
    { 2, 4, 61 },
    { 10, 24, 112 },
};

static const panel_style DOCK_STYLE = {
    STLXTHEME_DOCK_RADIUS, STLXTHEME_RIM, material::DOCK_RIM_FADE, DOCK_SHADOW_CONTACT_ALPHA, DOCK_SHADOW,
    sizeof(DOCK_SHADOW) / sizeof(DOCK_SHADOW[0]), DOCK_SHADOW_SIDE, DOCK_SHADOW_TOP, DOCK_BOTTOM_MARGIN,
};

dock_layout lay_out_dock(int32_t output_w, int32_t output_h, uint32_t launcher_count) {
    int32_t count = static_cast<int32_t>(launcher_count);
    int32_t tiles_w = count * TILE_SIZE + std::max(count - 1, 0) * TILE_GAP;
    int32_t shelf_w = count > 0 ? tiles_w + 2 * DOCK_PADDING : 0;
    int32_t shelf_y = output_h - DOCK_BOTTOM_MARGIN - STLXTHEME_DOCK_HEIGHT;

    dock_layout layout;
    layout.shelf = { (output_w - shelf_w) / 2, shelf_y, shelf_w, STLXTHEME_DOCK_HEIGHT };
    layout.band_y = shelf_y - DOCK_SHADOW_TOP;
    layout.band_h = output_h - layout.band_y;

    return layout;
}

damage_list::rect dock_tile_rect(const dock_layout& layout, uint32_t index) {
    int32_t x = layout.shelf.x + DOCK_PADDING + static_cast<int32_t>(index) * (TILE_SIZE + TILE_GAP);
    return { x, layout.shelf.y + DOCK_PADDING, TILE_SIZE, TILE_SIZE };
}

bool launches_app(const stlxconf_pin_t& pin, const char* app_id) {
    const char* slash = strrchr(pin.path, '/');
    const char* name = slash ? slash + 1 : pin.path;

    return strcmp(name, app_id) == 0;
}

stlxgfx_surface_t* load_launcher_icon(const stlxconf_pin_t& pin) {
    stlxgfx_surface_t* icon = pin.icon_path[0] ? stlxgfx_load_image(pin.icon_path) : nullptr;
    if (!icon) {
        icon = stlxgfx_load_image(STLXCONF_DEFAULT_ICON);
    }

    if (!icon) {
        return nullptr;
    }

    bool is_icon_size = icon->width == ICON_SIZE && icon->height == ICON_SIZE;
    if (is_icon_size) {
        return icon;
    }

    stlxgfx_surface_t* scaled = stlxgfx_create_surface(ICON_SIZE, ICON_SIZE, 32, 16, 8, 0);
    if (scaled) {
        scaled->flags = icon->flags;
        stlxgfx_blit_scaled(scaled, 0, 0, ICON_SIZE, ICON_SIZE, icon, 0, 0, icon->width, icon->height);
    }

    stlxgfx_destroy_surface(icon);

    return scaled;
}

/* A copy of `rows` rows of `source` from `first_row`, its pixels opaque, so the toolkit's blend shows them whole */
static stlxgfx_surface_t* copy_opaque_rows(const stlxgfx_surface_t* source, int32_t first_row, uint32_t rows) {
    stlxgfx_surface_t* copy = stlxgfx_create_surface(source->width, rows, 32, 16, 8, 0);
    if (!copy) {
        return nullptr;
    }

    stlxgfx_blit(copy, 0, 0, source, 0, first_row, source->width, rows);
    for (uint32_t y = 0; y < rows; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(copy->pixels + static_cast<size_t>(y) * copy->pitch);
        for (uint32_t x = 0; x < copy->width; x++) {
            row[x] |= 0xFF000000u;
        }
    }

    return copy;
}

dock_band::dock_band(const stlxgfx_surface_t* backdrop, const stlxgfx_surface_t* frost, const dock_layout& layout)
    : ui::box(ui::axis::row) {
    m_picture = copy_opaque_rows(backdrop, layout.band_y, static_cast<uint32_t>(layout.band_h));
    if (!m_picture) {
        return;
    }

    const damage_list::rect& shelf = layout.shelf;
    if (shelf.w == 0) {
        return;
    }

    panel_picture glass;
    if (!glass.make(nullptr, DOCK_STYLE, shelf.w, shelf.h, shelf.x, shelf.y, frost)) {
        stlxgfx_destroy_surface(m_picture);
        m_picture = nullptr;
        return;
    }

    stlxgfx_blit(
        glass.content(),
        0,
        0,
        glass.background(),
        0,
        0,
        static_cast<uint32_t>(shelf.w),
        static_cast<uint32_t>(shelf.h)
    );
    glass.round_corners({ { 0, 0, shelf.w, shelf.h } });

    ui::rect box = glass.box();
    const stlxgfx_surface_t* shadowed = glass.surface();
    stlxgfx_blit_alpha(
        m_picture,
        shelf.x - box.x,
        shelf.y - layout.band_y - box.y,
        shadowed,
        0,
        0,
        shadowed->width,
        shadowed->height
    );

    damage_list::rect first_tile = dock_tile_rect(layout, 0);
    m_style.padding = { first_tile.y - layout.band_y, 0, 0, first_tile.x };
    m_style.gap = TILE_GAP;
    m_style.align_items = ui::align::start;
}

dock_band::~dock_band() {
    stlxgfx_destroy_surface(m_picture);
}

void dock_band::paint(ui::painter& p) {
    p.image({ 0, 0 }, m_picture);
}

void dock_tile::show_running(bool running, bool focused) {
    if (running != m_running || focused != m_focused) {
        m_running = running;
        m_focused = focused;
        invalidate();
    }
}

ui::size dock_tile::measure(ui::size) {
    return { TILE_SIZE, TILE_SIZE };
}

void dock_tile::paint(ui::painter& p) {
    ui::rect tile = { 0, 0, m_frame.w, m_frame.h };
    if (m_pressed) {
        p.rounded_rect(tile, STLXTHEME_DOCK_TILE_RADIUS, STLXTHEME_SURFACE_PRESS);
    } else if (m_hover) {
        p.rounded_rect(tile, STLXTHEME_DOCK_TILE_RADIUS, STLXTHEME_DOCK_TILE_HOVER);
    }

    int32_t inset = (TILE_SIZE - ICON_SIZE) / 2;
    if (m_icon) {
        p.image({ inset, inset }, m_icon);
    } else {
        char letter[2] = { m_pin.label[0] ? m_pin.label[0] : '?', '\0' };
        ui::size size = p.measure_text(letter, STLXTHEME_PAGE_TITLE_SIZE);
        int32_t baseline = (tile.h - size.h) / 2 + p.font_ascent(STLXTHEME_PAGE_TITLE_SIZE);
        p.text({ (tile.w - size.w) / 2, baseline }, letter, STLXTHEME_PAGE_TITLE_SIZE, STLXTHEME_TEXT_SECONDARY);
    }

    if (m_running) {
        int32_t width = m_focused ? TICK_FOCUSED_WIDTH : TICK_RUNNING_WIDTH;
        ui::rect tick = { (tile.w - width) / 2, tile.h - TICK_BOTTOM_INSET - TICK_HEIGHT, width, TICK_HEIGHT };
        p.rounded_rect(tick, TICK_RADIUS, STLXTHEME_ACCENT);
    }
}

void dock_tile::on_pointer_enter() {
    m_hover = true;
    invalidate();

    if (on_hover) {
        on_hover(m_index, true);
    }
}

void dock_tile::on_pointer_leave() {
    m_hover = false;
    m_pressed = false;
    invalidate();

    if (on_hover) {
        on_hover(m_index, false);
    }
}

bool dock_tile::on_pointer_down(const ui::pointer_event&) {
    m_pressed = true;
    invalidate();

    return true;
}

bool dock_tile::on_pointer_up(const ui::pointer_event& e) {
    bool is_inside = e.pos.x >= 0 && e.pos.y >= 0 && e.pos.x < m_frame.w && e.pos.y < m_frame.h;
    bool fires = m_pressed && is_inside && m_pin.path[0] != '\0';

    m_pressed = false;
    invalidate();

    if (fires && on_launch) {
        on_launch(m_pin);
    }

    return true;
}
