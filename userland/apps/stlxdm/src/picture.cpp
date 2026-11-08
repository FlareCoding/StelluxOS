#include "picture.hpp"
#include "server.hpp"

#include <stlxgfx/surface.h>

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
