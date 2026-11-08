#include "picture.hpp"
#include "server.hpp"

#include <stlxgfx/surface.h>

#include <algorithm>
#include <cstring>

static_assert(frame_picture::STRIPS == decor::FRAME_STRIPS);

constexpr uint32_t BYTES_PER_PIXEL = 4;

picture::picture(uint8_t* pixels, uint64_t address, size_t bytes)
    : m_pixels(pixels), m_address(address), m_bytes(bytes) {}

static size_t strip_bytes(const damage_list::rect& strip) {
    return static_cast<size_t>(strip.w) * static_cast<size_t>(strip.h) * BYTES_PER_PIXEL;
}

static bool draw_strip(
    uint8_t* pixels,
    const damage_list::rect& strip,
    const dm_window& w,
    const decor::chrome_state& st
) {
    stlxgfx_surface_t* surface = stlxgfx_surface_from_buffer(
        pixels,
        static_cast<uint32_t>(strip.w),
        static_cast<uint32_t>(strip.h),
        static_cast<uint32_t>(strip.w) * BYTES_PER_PIXEL,
        32,
        16,
        8,
        0
    );
    if (!surface) {
        return false;
    }

    surface->flags |= STLXGFX_SURFACE_STRAIGHT_ALPHA;
    decor::canvas canvas = { surface, strip.x, strip.y };
    decor::draw(canvas, w, st, strip);
    stlxgfx_destroy_surface(surface);

    return true;
}

static void clear_inside(
    stlxgfx_surface_t* surface,
    const std::vector<damage_list::rect>& drawn,
    const damage_list::rect& r
) {
    for (const damage_list::rect& bounds : drawn) {
        damage_list::rect stale = damage_list::intersect(bounds, r);
        if (stale.w > 0 && stale.h > 0) {
            stlxgfx_fill_rect(surface, stale.x, stale.y, static_cast<uint32_t>(stale.w),
                              static_cast<uint32_t>(stale.h), 0);
        }
    }
}

bool frame_picture::update(picture_maker& maker, const dm_window& w, const decor::chrome_state& st) {
    const dm_buffer& b = w.buffers[static_cast<size_t>(w.current)];
    bool is_same_size = b.width == m_width && b.height == m_height;
    bool is_same_title = memcmp(w.title, m_title, sizeof(m_title)) == 0;
    bool is_same_state = st == m_state;
    if (m_is_drawn && is_same_size && is_same_title && is_same_state) {
        return true;
    }

    m_is_drawn = false;
    damage_list::rect strips[STRIPS];
    decor::frame_strips(w, strips);

    size_t bytes = 0;
    for (uint32_t i = 0; i < STRIPS; i++) {
        m_offsets[i] = bytes;
        bytes += strip_bytes(strips[i]);
    }

    /* No compose still reads the old memory, since stlxdm waits for each one */
    if (!m_memory || m_memory->bytes() < bytes) {
        m_memory.reset();
        m_memory = maker.make_picture(bytes);
        if (!m_memory) {
            return false;
        }
    }

    memset(m_memory->pixels(), 0, bytes);
    for (uint32_t i = 0; i < STRIPS; i++) {
        bool is_empty = strips[i].w <= 0 || strips[i].h <= 0;
        if (!is_empty && !draw_strip(m_memory->pixels() + m_offsets[i], strips[i], w, st)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < STRIPS; i++) {
        m_strips[i] = { strips[i].x - w.x, strips[i].y - w.y, strips[i].w, strips[i].h };
    }

    m_width = b.width;
    m_height = b.height;
    m_state = st;
    memcpy(m_title, w.title, sizeof(m_title));
    m_is_drawn = true;

    return true;
}

void frame_picture::place(const dm_window& w, int32_t origin_x, int32_t origin_y, layer_window& shown) const {
    for (uint32_t i = 0; i < STRIPS; i++) {
        const damage_list::rect& strip = m_strips[i];
        int32_t x = w.x + strip.x - origin_x;
        int32_t y = w.y + strip.y - origin_y;

        shown.frame[i].bounds = { x, y, strip.w, strip.h };
        shown.frame[i].picture = {
            m_memory->address() + m_offsets[i],
            static_cast<uint32_t>(strip.w) * BYTES_PER_PIXEL,
            x,
            y
        };
    }
}

std::unique_ptr<top_picture> top_picture::make(picture_maker& maker, uint32_t width, uint32_t height) {
    uint32_t pitch = width * BYTES_PER_PIXEL;
    size_t bytes = static_cast<size_t>(pitch) * height;

    std::unique_ptr<top_picture> made(new top_picture());
    made->m_memory = maker.make_picture(bytes);
    if (!made->m_memory) {
        return nullptr;
    }

    made->m_surface = stlxgfx_surface_from_buffer(made->m_memory->pixels(), width, height, pitch, 32, 16, 8, 0);
    if (!made->m_surface) {
        return nullptr;
    }

    made->m_surface->flags |= STLXGFX_SURFACE_STRAIGHT_ALPHA;
    memset(made->m_memory->pixels(), 0, bytes);

    return made;
}

top_picture::~top_picture() {
    stlxgfx_destroy_surface(m_surface);
}

void top_picture::begin() {
    std::swap(m_drawn, m_last_drawn);
    m_drawn.clear();
}

void top_picture::add_bounds(const damage_list::rect& bounds) {
    int32_t width = static_cast<int32_t>(m_surface->width);
    int32_t height = static_cast<int32_t>(m_surface->height);
    damage_list::rect inside = damage_list::intersect(bounds, { 0, 0, width, height });
    if (inside.w > 0 && inside.h > 0) {
        m_drawn.push_back(inside);
    }
}

void top_picture::clear(const damage_list::rect& r) {
    clear_inside(m_surface, m_last_drawn, r);
    clear_inside(m_surface, m_drawn, r);
}

layer_piece top_picture::layer() const {
    if (m_drawn.empty()) {
        return {};
    }

    int32_t x0 = m_drawn[0].x;
    int32_t y0 = m_drawn[0].y;
    int32_t x1 = x0 + m_drawn[0].w;
    int32_t y1 = y0 + m_drawn[0].h;
    for (const damage_list::rect& bounds : m_drawn) {
        x0 = std::min(x0, bounds.x);
        y0 = std::min(y0, bounds.y);
        x1 = std::max(x1, bounds.x + bounds.w);
        y1 = std::max(y1, bounds.y + bounds.h);
    }

    return { { x0, y0, x1 - x0, y1 - y0 }, { m_memory->address(), m_surface->pitch, 0, 0 } };
}

std::unique_ptr<backdrop_picture> backdrop_picture::make(picture_maker& maker, uint32_t width, uint32_t height) {
    uint32_t pitch = width * BYTES_PER_PIXEL;
    size_t bytes = static_cast<size_t>(pitch) * height;

    std::unique_ptr<backdrop_picture> made(new backdrop_picture());
    made->m_memory = maker.make_picture(bytes);
    if (!made->m_memory) {
        return nullptr;
    }

    made->m_twin = maker.make_twin(bytes);
    if (!made->m_twin) {
        return nullptr;
    }

    made->m_surface = stlxgfx_surface_from_buffer(made->m_memory->pixels(), width, height, pitch, 32, 16, 8, 0);
    if (!made->m_surface) {
        return nullptr;
    }

    made->mark_rows(0, static_cast<int32_t>(height));

    return made;
}

backdrop_picture::~backdrop_picture() {
    stlxgfx_destroy_surface(m_surface);
}

layer_picture backdrop_picture::twin_layer() const {
    return { m_twin->address(), m_surface->pitch, 0, 0 };
}

void backdrop_picture::mark_rows(int32_t y, int32_t count) {
    int64_t end = static_cast<int64_t>(y) + count;
    int32_t first_row = std::max(y, 0);
    int32_t end_row = static_cast<int32_t>(std::min(end, static_cast<int64_t>(m_surface->height)));
    if (first_row >= end_row) {
        return;
    }

    /* Every span that overlaps or touches the added rows merges into them */
    row_span added = { first_row, end_row };
    auto first = std::find_if(m_changed.begin(), m_changed.end(), [&](const row_span& s) {
        return s.end >= added.first;
    });
    auto last = first;
    while (last != m_changed.end() && last->first <= added.end) {
        added.first = std::min(added.first, last->first);
        added.end = std::max(added.end, last->end);
        last++;
    }

    first = m_changed.erase(first, last);
    m_changed.insert(first, added);
}

void backdrop_picture::take_copies(std::vector<picture_copy>& copies) {
    size_t pitch = m_surface->pitch;
    for (const row_span& s : m_changed) {
        copies.push_back({ static_cast<size_t>(s.first) * pitch, static_cast<size_t>(s.end - s.first) * pitch });
    }

    m_changed.clear();
}
