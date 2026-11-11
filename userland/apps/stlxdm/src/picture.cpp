#include "picture.hpp"
#include "server.hpp"

#include <stlxgfx/surface.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

static_assert(frame_picture::STRIPS == decor::FRAME_STRIPS);

constexpr uint32_t BYTES_PER_PIXEL = 4;

/* A resize keeps memory holding at most this many times the bytes its new size needs */
constexpr size_t MEMORY_OVER_NEED_MAX = 4;

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

/* Clears the parts of `drawn` inside `r` and marks them, where this compose's draws in `r` change pixels as well */
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
            picture.mark_changed(stale);
        }
    }
}

/* Gives `picture` the size of `strip`, keeping its memory when `is_same_maker` says `maker` made it and its `resize`
 * accepts the strip, else making it anew, with room to grow when it replaces memory. False when memory runs out */
static bool fit_strip(
    std::unique_ptr<twinned_picture>& picture,
    picture_maker& maker,
    bool is_same_maker,
    const damage_list::rect& strip
) {
    uint32_t width = static_cast<uint32_t>(strip.w);
    uint32_t height = static_cast<uint32_t>(strip.h);
    bool is_reusable = is_same_maker && picture;
    if (is_reusable && picture->resize(width, height)) {
        return true;
    }

    /* No compose still reads the old memory, since frames are drawn only after the last compose ended */
    picture.reset();
    size_t bytes = static_cast<size_t>(width) * height * BYTES_PER_PIXEL;
    picture = twinned_picture::make(maker, width, height, is_reusable ? room_to_grow(bytes) : 0);

    return picture != nullptr;
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
            bool is_empty = redrawn.w <= 0 || redrawn.h <= 0;
            if (!m_pictures[i] || is_empty) {
                continue;
            }

            if (!draw_strip(m_pictures[i]->pixels(), placed, redrawn, w, st)) {
                return false;
            }

            m_pictures[i]->mark_changed({ redrawn.x - placed.x, redrawn.y - placed.y, redrawn.w, redrawn.h });
        }

        m_state = st;
        m_is_drawn = true;
        return true;
    }

    damage_list::rect strips[STRIPS];
    decor::frame_strips(w, st.focused, strips);

    for (uint32_t i = 0; i < STRIPS; i++) {
        if (strips[i].w <= 0 || strips[i].h <= 0) {
            m_pictures[i].reset();
            continue;
        }

        if (!fit_strip(m_pictures[i], maker, is_same_maker, strips[i])) {
            return false;
        }

        memset(m_pictures[i]->pixels(), 0, static_cast<size_t>(m_pictures[i]->pitch()) * m_pictures[i]->height());
        if (!draw_strip(m_pictures[i]->pixels(), strips[i], strips[i], w, st)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < STRIPS; i++) {
        m_strips[i] = { strips[i].x - w.x, strips[i].y - w.y, strips[i].w, strips[i].h };
    }

    m_maker = &maker;
    m_width = b.width;
    m_height = b.height;
    m_state = st;
    memcpy(m_title, w.title, sizeof(m_title));
    m_is_drawn = true;

    return true;
}

void frame_picture::place(const dm_window& w, int32_t origin_x, int32_t origin_y, layer_window& shown) const {
    for (uint32_t i = 0; i < STRIPS; i++) {
        twinned_picture* picture = m_pictures[i].get();
        if (!picture) {
            shown.frame[i] = {};
            continue;
        }

        damage_list::rect placed = placed_strip(w, m_strips[i]);
        int32_t x = placed.x - origin_x;
        int32_t y = placed.y - origin_y;
        shown.frame[i].bounds = { x, y, m_strips[i].w, m_strips[i].h };
        shown.frame[i].picture = { picture->address(), picture->pitch(), x, y, picture };
    }
}

void frame_picture::blend(const decor::canvas& back, const dm_window& w, const damage_list::rect& clip) const {
    for (uint32_t i = 0; i < STRIPS; i++) {
        const damage_list::rect& strip = m_strips[i];
        damage_list::rect placed = placed_strip(w, strip);
        damage_list::rect shown = damage_list::intersect(placed, clip);
        bool is_empty = shown.w <= 0 || shown.h <= 0;
        if (!m_pictures[i] || is_empty) {
            continue;
        }

        stlxgfx_surface_t* source = make_strip_surface(m_pictures[i]->pixels(), strip);
        if (!source) {
            continue;
        }

        stlxgfx_blit_alpha(back.surface, shown.x - back.x, shown.y - back.y, source, shown.x - placed.x,
                           shown.y - placed.y, static_cast<uint32_t>(shown.w), static_cast<uint32_t>(shown.h));
        stlxgfx_destroy_surface(source);
    }
}

picture_region frame_picture::strip_pixels(uint32_t index) const {
    const damage_list::rect& strip = m_strips[index];
    twinned_picture* picture = m_pictures[index].get();
    if (!picture) {
        return {};
    }

    picture_region region;
    region.pixels = picture->pixels();
    region.address = picture->address();
    region.twinned = picture;

    region.pitch = picture->pitch();
    region.width = strip.w;
    region.height = strip.h;

    return region;
}

std::unique_ptr<top_picture> top_picture::make(picture_maker& maker, uint32_t width, uint32_t height) {
    std::unique_ptr<top_picture> made(new top_picture());
    made->m_picture = twinned_picture::make(maker, width, height);
    if (!made->m_picture) {
        return nullptr;
    }

    uint32_t pitch = made->m_picture->pitch();
    made->m_surface = stlxgfx_surface_from_buffer(made->m_picture->pixels(), width, height, pitch, 32, 16, 8, 0);
    if (!made->m_surface) {
        return nullptr;
    }

    made->m_surface->flags |= STLXGFX_SURFACE_STRAIGHT_ALPHA;
    memset(made->m_picture->pixels(), 0, static_cast<size_t>(pitch) * height);

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
    m_picture->mark_changed(r);
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
    std::unique_ptr<backdrop_picture> made(new backdrop_picture());
    made->m_picture = twinned_picture::make(maker, width, height);
    if (!made->m_picture || !made->m_picture->has_twin()) {
        return nullptr;
    }

    uint32_t pitch = made->m_picture->pitch();
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
    m_picture->mark_changed({ 0, y, static_cast<int32_t>(m_surface->width), count });
}

/* Adds the part of `r` inside a picture `width` by `height` to `changed`, summing in 64 bits since a window's damage
 * rects come from its client unchecked */
static void mark_inside(damage_list& changed, const damage_list::rect& r, uint32_t width, uint32_t height) {
    int64_t left = std::max<int64_t>(r.x, 0);
    int64_t top = std::max<int64_t>(r.y, 0);
    int64_t right = std::min<int64_t>(static_cast<int64_t>(r.x) + r.w, width);
    int64_t bottom = std::min<int64_t>(static_cast<int64_t>(r.y) + r.h, height);
    if (left >= right || top >= bottom) {
        return;
    }

    changed.add(
        static_cast<int32_t>(left),
        static_cast<int32_t>(top),
        static_cast<int32_t>(right - left),
        static_cast<int32_t>(bottom - top)
    );
}

/* Appends the rects of `changed`, or the whole picture `width` by `height` once more than fit were marked, and clears
 * it */
static void take_changed(
    damage_list& changed,
    uint32_t width,
    uint32_t height,
    std::vector<damage_list::rect>& copies
) {
    if (changed.full()) {
        copies.push_back({ 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height) });
    } else {
        for (uint32_t i = 0; i < changed.count(); i++) {
            copies.push_back(changed.at(i));
        }
    }

    changed.clear();
}

/* Whether `memory` holds `bytes` without holding over `MEMORY_OVER_NEED_MAX` times as many */
static bool fits_memory(size_t bytes, size_t memory) {
    return bytes <= memory && bytes * MEMORY_OVER_NEED_MAX >= memory;
}

/* Makes `memory` of `bytes`, and `twin` of as many when `maker` makes twins. False when memory runs out */
static bool make_memory(
    picture_maker& maker,
    size_t bytes,
    std::unique_ptr<picture>& memory,
    std::unique_ptr<picture>& twin
) {
    twin.reset();
    memory.reset();
    memory = maker.make_picture(bytes);
    if (!memory) {
        return false;
    }

    if (maker.makes_twins()) {
        twin = maker.make_twin(bytes);
    }

    return twin || !maker.makes_twins();
}

size_t room_to_grow(size_t bytes) {
    return bytes + bytes / 2;
}

std::unique_ptr<twinned_picture> twinned_picture::make(
    picture_maker& maker,
    uint32_t width,
    uint32_t height,
    size_t min_bytes
) {
    size_t needed = static_cast<size_t>(width) * height * BYTES_PER_PIXEL;
    std::unique_ptr<twinned_picture> made(new twinned_picture());
    bool is_made = make_memory(maker, std::max(needed, min_bytes), made->m_memory, made->m_twin);
    if (!is_made && min_bytes > needed) {
        is_made = make_memory(maker, needed, made->m_memory, made->m_twin);
    }

    if (!is_made) {
        return nullptr;
    }

    made->m_width = width;
    made->m_height = height;
    made->m_pitch = width * BYTES_PER_PIXEL;
    made->m_changed.add_full();

    return made;
}

uint64_t twinned_picture::address() const {
    return m_twin ? m_twin->address() : m_memory->address();
}

bool twinned_picture::resize(uint32_t width, uint32_t height) {
    if (!fits_memory(static_cast<size_t>(width) * height * BYTES_PER_PIXEL, m_memory->bytes())) {
        return false;
    }

    m_width = width;
    m_height = height;
    m_pitch = width * BYTES_PER_PIXEL;
    m_changed.clear();
    m_changed.add_full();

    return true;
}

void twinned_picture::mark_changed(const damage_list::rect& r) {
    if (m_twin) {
        mark_inside(m_changed, r, m_width, m_height);
    }
}

void twinned_picture::take_copies(std::vector<damage_list::rect>& copies) {
    take_changed(m_changed, m_width, m_height, copies);
}

std::unique_ptr<window_twin> window_twin::make(
    picture_maker& maker,
    uint32_t pitch,
    uint32_t width,
    uint32_t height,
    size_t min_bytes
) {
    if (!maker.makes_twins()) {
        return nullptr;
    }

    size_t needed = static_cast<size_t>(pitch) * height;
    std::unique_ptr<window_twin> made(new window_twin());
    made->m_twin = maker.make_twin(std::max(needed, min_bytes));
    if (!made->m_twin && min_bytes > needed) {
        made->m_twin = maker.make_twin(needed);
    }

    if (!made->m_twin) {
        return nullptr;
    }

    made->m_pitch = pitch;
    made->m_width = width;
    made->m_height = height;
    made->m_changed.add_full();

    return made;
}

bool window_twin::resize(uint32_t pitch, uint32_t width, uint32_t height) {
    if (!fits_memory(static_cast<size_t>(pitch) * height, m_twin->bytes())) {
        return false;
    }

    m_pitch = pitch;
    m_width = width;
    m_height = height;
    m_changed.clear();
    m_changed.add_full();

    return true;
}

void window_twin::mark_changed(const damage_list::rect& r) {
    mark_inside(m_changed, r, m_width, m_height);
}

void window_twin::take_copies(std::vector<damage_list::rect>& copies) {
    take_changed(m_changed, m_width, m_height, copies);
}
