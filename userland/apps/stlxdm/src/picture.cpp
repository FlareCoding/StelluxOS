#include "picture.hpp"
#include "server.hpp"

#include <stlxgfx/surface.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

static_assert(frame_picture::STRIPS == decor::FRAME_STRIPS);

constexpr uint32_t BYTES_PER_PIXEL = 4;

namespace {

/* A picture in stlxdm's heap, which no GPU reads */
class heap_picture : public picture {
public:
    heap_picture(uint8_t* pixels, size_t bytes) : picture(pixels, 0, bytes) {}
    ~heap_picture() override { free(pixels()); }
};

} // namespace

picture::picture(uint8_t* pixels, uint64_t address, size_t bytes)
    : m_pixels(pixels), m_address(address), m_bytes(bytes) {}

bool picture_maker::makes_twins() const {
    return false;
}

std::unique_ptr<picture> picture_maker::make_twin(size_t) {
    return nullptr;
}

std::unique_ptr<picture> heap_picture_maker::make_picture(size_t bytes) {
    uint8_t* pixels = static_cast<uint8_t*>(malloc(bytes));
    if (!pixels) {
        return nullptr;
    }

    return std::make_unique<heap_picture>(pixels, bytes);
}

static size_t strip_bytes(const damage_list::rect& strip) {
    return static_cast<size_t>(strip.w) * static_cast<size_t>(strip.h) * BYTES_PER_PIXEL;
}

static damage_list::rect placed_strip(const dm_window& w, const damage_list::rect& strip) {
    return { w.x + strip.x, w.y + strip.y, strip.w, strip.h };
}

static stlxgfx_surface_t* make_strip_surface(uint8_t* pixels, const damage_list::rect& strip) {
    return stlxgfx_surface_from_buffer(
        pixels,
        static_cast<uint32_t>(strip.w),
        static_cast<uint32_t>(strip.h),
        static_cast<uint32_t>(strip.w) * BYTES_PER_PIXEL,
        32,
        16,
        8,
        0
    );
}

/* Draws the frame of `w` into a strip's pixels inside `clip`, which must
 * lie in desktop coordinates inside `strip` */
static bool draw_strip(
    uint8_t* pixels,
    const damage_list::rect& strip,
    const damage_list::rect& clip,
    const dm_window& w,
    const decor::chrome_state& st
) {
    stlxgfx_surface_t* surface = make_strip_surface(pixels, strip);
    if (!surface) {
        return false;
    }

    surface->flags |= STLXGFX_SURFACE_STRAIGHT_ALPHA;
    decor::canvas canvas = { surface, strip.x, strip.y };
    decor::draw(canvas, w, st, clip);
    stlxgfx_destroy_surface(surface);

    return true;
}

/* Marks `count` rows from row `y` changed in the image `width` pixels wide that starts `offset` bytes into `picture` */
static void mark_image_rows(twinned_picture& picture, size_t offset, int32_t width, int32_t y, int32_t count) {
    size_t pitch = static_cast<size_t>(width) * BYTES_PER_PIXEL;
    picture.mark_changed(offset + static_cast<size_t>(y) * pitch, static_cast<size_t>(count) * pitch);
}

/* Clears the parts of `drawn` inside `r` and marks their rows, which this compose's draws in `r` change as well */
static void clear_inside(
    twinned_picture& picture,
    stlxgfx_surface_t* surface,
    const std::vector<damage_list::rect>& drawn,
    const damage_list::rect& r
) {
    for (const damage_list::rect& bounds : drawn) {
        damage_list::rect stale = damage_list::intersect(bounds, r);
        if (stale.w > 0 && stale.h > 0) {
            stlxgfx_fill_rect(surface, stale.x, stale.y, static_cast<uint32_t>(stale.w),
                              static_cast<uint32_t>(stale.h), 0);
            mark_image_rows(picture, 0, static_cast<int32_t>(surface->width), stale.y, stale.h);
        }
    }
}

bool frame_picture::update(picture_maker& maker, const dm_window& w, const decor::chrome_state& st) {
    const dm_buffer& b = w.buffers[static_cast<size_t>(w.current)];
    bool is_same_size = b.width == m_width && b.height == m_height;
    bool is_same_title = memcmp(w.title, m_title, sizeof(m_title)) == 0;
    bool is_same_maker = &maker == m_maker;
    bool is_same_frame = m_is_drawn && is_same_maker && is_same_size && is_same_title && st.focused == m_state.focused;
    if (is_same_frame && st == m_state) {
        return true;
    }

    m_is_drawn = false;

    /* A hover or press changes only the controls, so only their part of
     * the strips is drawn again */
    if (is_same_frame) {
        damage_list::rect controls = decor::controls_rect(w);
        for (uint32_t i = 0; i < STRIPS; i++) {
            damage_list::rect placed = placed_strip(w, m_strips[i]);
            damage_list::rect redrawn = damage_list::intersect(placed, controls);
            if (redrawn.w <= 0 || redrawn.h <= 0) {
                continue;
            }

            if (!draw_strip(m_memory->pixels() + m_offsets[i], placed, redrawn, w, st)) {
                return false;
            }

            mark_image_rows(*m_memory, m_offsets[i], placed.w, redrawn.y - placed.y, redrawn.h);
        }

        m_state = st;
        m_is_drawn = true;
        return true;
    }

    damage_list::rect strips[STRIPS];
    decor::frame_strips(w, st.focused, strips);

    size_t bytes = 0;
    for (uint32_t i = 0; i < STRIPS; i++) {
        m_offsets[i] = bytes;
        bytes += strip_bytes(strips[i]);
    }

    /* No compose still reads the old memory, since frames are drawn only after the last compose ended */
    bool has_room = m_memory && m_memory->bytes() >= bytes;
    if (!is_same_maker || !has_room) {
        m_memory.reset();
        m_memory = twinned_picture::make(maker, bytes);
        if (!m_memory) {
            return false;
        }

        m_maker = &maker;
    }

    memset(m_memory->pixels(), 0, bytes);
    m_memory->mark_changed(0, bytes);
    for (uint32_t i = 0; i < STRIPS; i++) {
        bool is_empty = strips[i].w <= 0 || strips[i].h <= 0;
        if (!is_empty && !draw_strip(m_memory->pixels() + m_offsets[i], strips[i], strips[i], w, st)) {
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
        damage_list::rect placed = placed_strip(w, strip);
        int32_t x = placed.x - origin_x;
        int32_t y = placed.y - origin_y;

        shown.frame[i].bounds = { x, y, strip.w, strip.h };
        shown.frame[i].picture = {
            m_memory->address() + m_offsets[i],
            static_cast<uint32_t>(strip.w) * BYTES_PER_PIXEL,
            x,
            y,
            m_memory.get()
        };
    }
}

void frame_picture::blend(const decor::canvas& back, const dm_window& w, const damage_list::rect& clip) const {
    for (uint32_t i = 0; i < STRIPS; i++) {
        const damage_list::rect& strip = m_strips[i];
        damage_list::rect placed = placed_strip(w, strip);
        damage_list::rect shown = damage_list::intersect(placed, clip);
        if (shown.w <= 0 || shown.h <= 0) {
            continue;
        }

        stlxgfx_surface_t* source = make_strip_surface(m_memory->pixels() + m_offsets[i], strip);
        if (!source) {
            continue;
        }

        stlxgfx_blit_alpha(back.surface, shown.x - back.x, shown.y - back.y, source, shown.x - placed.x,
                           shown.y - placed.y, static_cast<uint32_t>(shown.w), static_cast<uint32_t>(shown.h));
        stlxgfx_destroy_surface(source);
    }
}

/* A picture in memory only the CPU reads has no GPU address to offset */
picture_region frame_picture::strip_pixels(uint32_t index) const {
    const damage_list::rect& strip = m_strips[index];
    uint64_t address = m_memory->address();

    picture_region region;
    region.pixels = m_memory->pixels() + m_offsets[index];
    region.address = address != 0 ? address + m_offsets[index] : 0;
    region.twinned = m_memory.get();

    region.pitch = static_cast<uint32_t>(strip.w) * BYTES_PER_PIXEL;
    region.width = strip.w;
    region.height = strip.h;

    return region;
}

std::unique_ptr<top_picture> top_picture::make(picture_maker& maker, uint32_t width, uint32_t height) {
    uint32_t pitch = width * BYTES_PER_PIXEL;
    size_t bytes = static_cast<size_t>(pitch) * height;

    std::unique_ptr<top_picture> made(new top_picture());
    made->m_picture = twinned_picture::make(maker, bytes);
    if (!made->m_picture) {
        return nullptr;
    }

    made->m_surface = stlxgfx_surface_from_buffer(made->m_picture->pixels(), width, height, pitch, 32, 16, 8, 0);
    if (!made->m_surface) {
        return nullptr;
    }

    made->m_surface->flags |= STLXGFX_SURFACE_STRAIGHT_ALPHA;
    memset(made->m_picture->pixels(), 0, bytes);

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
    clear_inside(*m_picture, m_surface, m_last_drawn, r);
    clear_inside(*m_picture, m_surface, m_drawn, r);
}

void top_picture::mark_drawn(const damage_list::rect& r) {
    int32_t width = static_cast<int32_t>(m_surface->width);
    int32_t height = static_cast<int32_t>(m_surface->height);
    damage_list::rect inside = damage_list::intersect(r, { 0, 0, width, height });
    if (inside.w > 0 && inside.h > 0) {
        mark_image_rows(*m_picture, 0, width, inside.y, inside.h);
    }
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

    return { { x0, y0, x1 - x0, y1 - y0 }, { m_picture->address(), m_surface->pitch, 0, 0, m_picture.get() } };
}

std::unique_ptr<backdrop_picture> backdrop_picture::make(picture_maker& maker, uint32_t width, uint32_t height) {
    uint32_t pitch = width * BYTES_PER_PIXEL;
    size_t bytes = static_cast<size_t>(pitch) * height;

    std::unique_ptr<backdrop_picture> made(new backdrop_picture());
    made->m_picture = twinned_picture::make(maker, bytes);
    if (!made->m_picture || !made->m_picture->has_twin()) {
        return nullptr;
    }

    made->m_surface = stlxgfx_surface_from_buffer(made->m_picture->pixels(), width, height, pitch, 32, 16, 8, 0);
    if (!made->m_surface) {
        return nullptr;
    }

    return made;
}

backdrop_picture::~backdrop_picture() {
    stlxgfx_destroy_surface(m_surface);
}

layer_picture backdrop_picture::twin_layer() const {
    return { m_picture->address(), m_surface->pitch, 0, 0, m_picture.get() };
}

void backdrop_picture::mark_rows(int32_t y, int32_t count) {
    int64_t end = static_cast<int64_t>(y) + count;
    int32_t first_row = std::max(y, 0);
    int32_t end_row = static_cast<int32_t>(std::min(end, static_cast<int64_t>(m_surface->height)));
    if (first_row < end_row) {
        mark_image_rows(*m_picture, 0, static_cast<int32_t>(m_surface->width), first_row, end_row - first_row);
    }
}

std::unique_ptr<twinned_picture> twinned_picture::make(picture_maker& maker, size_t bytes) {
    std::unique_ptr<twinned_picture> made(new twinned_picture());
    made->m_memory = maker.make_picture(bytes);
    if (!made->m_memory) {
        return nullptr;
    }

    if (maker.makes_twins()) {
        made->m_twin = maker.make_twin(bytes);
        if (!made->m_twin) {
            return nullptr;
        }
    }

    made->mark_changed(0, bytes);

    return made;
}

uint64_t twinned_picture::address() const {
    return m_twin ? m_twin->address() : m_memory->address();
}

void twinned_picture::mark_changed(size_t offset, size_t bytes) {
    size_t size = m_memory->bytes();
    bool is_empty = offset >= size || bytes == 0;
    if (!m_twin || is_empty) {
        return;
    }

    /* Every span that overlaps or touches the added bytes merges into them */
    byte_span added = { offset, offset + std::min(bytes, size - offset) };
    auto first = std::find_if(m_changed.begin(), m_changed.end(), [&](const byte_span& s) {
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

void twinned_picture::take_copies(std::vector<picture_copy>& copies) {
    for (const byte_span& s : m_changed) {
        copies.push_back({ s.first, s.end - s.first });
    }

    m_changed.clear();
}
