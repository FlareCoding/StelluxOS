#include "cursor.hpp"

#include <stlxgfx/bmp.h>

constexpr const char* ARROW_BMP_PATH = "/etc/res/cursors/pointer_22x22.bmp";
constexpr int32_t ARROW_BMP_HOT_X = 4;
constexpr int32_t ARROW_BMP_HOT_Y = 1;

constexpr uint32_t OUTLINE = 0xFF16161E;
constexpr uint32_t FILL = 0xFFF5F5F5;

/* Classic pointer, X for outline, dot for fill */
constexpr int32_t ARROW_W = 12;
constexpr int32_t ARROW_H = 18;
constexpr const char* ARROW_SHAPE[ARROW_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      XX    ",
    "            ",
};

static stlxgfx_surface_t* build_from_shape(const char* const* shape,
                                           int32_t w, int32_t h) {
    stlxgfx_surface_t* s = stlxgfx_create_surface(
        static_cast<uint32_t>(w), static_cast<uint32_t>(h), 32, 16, 8, 0);
    if (!s) {
        return nullptr;
    }

    stlxgfx_clear(s, 0x00000000);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w && shape[y][x]; x++) {
            if (shape[y][x] == 'X') {
                stlxgfx_fill_rect(s, x, y, 1, 1, OUTLINE);
            } else if (shape[y][x] == '.') {
                stlxgfx_fill_rect(s, x, y, 1, 1, FILL);
            }
        }
    }

    return s;
}

/* Bars and arrows assembled from rects on a transparent ground */
static stlxgfx_surface_t* build_ibeam() {
    stlxgfx_surface_t* s = stlxgfx_create_surface(7, 16, 32, 16, 8, 0);
    if (!s) {
        return nullptr;
    }

    stlxgfx_clear(s, 0x00000000);
    stlxgfx_fill_rect(s, 1, 0, 5, 2, FILL);
    stlxgfx_fill_rect(s, 3, 1, 1, 14, FILL);
    stlxgfx_fill_rect(s, 1, 14, 5, 2, FILL);
    return s;
}

static stlxgfx_surface_t* build_resize(bool horizontal) {
    uint32_t w = horizontal ? 17 : 7;
    uint32_t h = horizontal ? 7 : 17;
    stlxgfx_surface_t* s = stlxgfx_create_surface(w, h, 32, 16, 8, 0);
    if (!s) {
        return nullptr;
    }

    stlxgfx_clear(s, 0x00000000);
    if (horizontal) {
        stlxgfx_fill_rect(s, 2, 3, 13, 1, FILL);
        for (int32_t i = 0; i < 3; i++) {
            stlxgfx_fill_rect(s, 2 + i, 3 - i, 1,
                              static_cast<uint32_t>(2 * i + 1), FILL);
            stlxgfx_fill_rect(s, 14 - i, 3 - i, 1,
                              static_cast<uint32_t>(2 * i + 1), FILL);
        }
    } else {
        stlxgfx_fill_rect(s, 3, 2, 1, 13, FILL);
        for (int32_t i = 0; i < 3; i++) {
            stlxgfx_fill_rect(s, 3 - i, 2 + i,
                              static_cast<uint32_t>(2 * i + 1), 1, FILL);
            stlxgfx_fill_rect(s, 3 - i, 14 - i,
                              static_cast<uint32_t>(2 * i + 1), 1, FILL);
        }
    }

    return s;
}

/* A soft shadow at half the sprite's own coverage, drawn offset one
 * pixel down and right so the shape reads on any background */
static stlxgfx_surface_t* build_shadow(const stlxgfx_surface_t* sprite) {
    if (sprite->bpp != 32) {
        return nullptr;
    }

    stlxgfx_surface_t* shadow = stlxgfx_create_surface(
        sprite->width, sprite->height, 32, 16, 8, 0);
    if (!shadow) {
        return nullptr;
    }

    stlxgfx_clear(shadow, 0x00000000);
    uint8_t src_alpha = stlxgfx_alpha_byte_index(sprite);
    uint8_t dst_alpha = stlxgfx_alpha_byte_index(shadow);
    for (uint32_t y = 0; y < sprite->height; y++) {
        const uint8_t* sp = sprite->pixels + y * sprite->pitch;
        uint8_t* dp = shadow->pixels + y * shadow->pitch;
        for (uint32_t x = 0; x < sprite->width; x++) {
            dp[x * 4 + dst_alpha] =
                static_cast<uint8_t>(sp[x * 4 + src_alpha] / 2);
        }
    }

    return shadow;
}

static void destroy_sprite(cursor_sprite* s) {
    stlxgfx_destroy_surface(s->image);
    stlxgfx_destroy_surface(s->shadow);
    s->image = nullptr;
    s->shadow = nullptr;
}

int cursor::init() {
    m_arrow.image = stlxgfx_load_bmp(ARROW_BMP_PATH);
    if (m_arrow.image) {
        m_arrow.hot_x = ARROW_BMP_HOT_X;
        m_arrow.hot_y = ARROW_BMP_HOT_Y;
    } else {
        m_arrow.image = build_from_shape(ARROW_SHAPE, ARROW_W, ARROW_H);
    }

    m_ibeam.image = build_ibeam();
    m_ibeam.hot_x = 3;
    m_ibeam.hot_y = 8;

    m_resize_h.image = build_resize(true);
    m_resize_h.hot_x = 8;
    m_resize_h.hot_y = 3;

    m_resize_v.image = build_resize(false);
    m_resize_v.hot_x = 3;
    m_resize_v.hot_y = 8;

    cursor_sprite* all[] = { &m_arrow, &m_ibeam, &m_resize_h, &m_resize_v };
    for (cursor_sprite* s : all) {
        if (s->image) {
            s->shadow = build_shadow(s->image);
        }
    }

    return m_arrow.image ? 0 : -1;
}

void cursor::shutdown() {
    destroy_sprite(&m_arrow);
    destroy_sprite(&m_ibeam);
    destroy_sprite(&m_resize_h);
    destroy_sprite(&m_resize_v);
}

const cursor_sprite* cursor::sprite_for(uint32_t shape) const {
    switch (shape) {
    case SWP_CURSOR_IBEAM:    return &m_ibeam;
    case SWP_CURSOR_HAND:     return &m_arrow;
    case SWP_CURSOR_RESIZE_H: return &m_resize_h;
    case SWP_CURSOR_RESIZE_V: return &m_resize_v;
    case SWP_CURSOR_NONE:     return nullptr;
    default:                  return &m_arrow;
    }
}

void cursor::bounds(uint32_t shape, int32_t x, int32_t y,
                    int32_t* bx, int32_t* by,
                    int32_t* bw, int32_t* bh) const {
    const cursor_sprite* s = sprite_for(shape);
    if (!s || !s->image) {
        *bx = x;
        *by = y;
        *bw = 0;
        *bh = 0;
        return;
    }

    /* The shadow extends coverage one pixel down and right */
    int32_t pad = s->shadow ? 1 : 0;
    *bx = x - s->hot_x;
    *by = y - s->hot_y;
    *bw = static_cast<int32_t>(s->image->width) + pad;
    *bh = static_cast<int32_t>(s->image->height) + pad;
}

void cursor::draw(stlxgfx_surface_t* back, uint32_t shape,
                  int32_t x, int32_t y) const {
    const cursor_sprite* s = sprite_for(shape);
    if (!s || !s->image) {
        return;
    }

    int32_t ox = x - s->hot_x;
    int32_t oy = y - s->hot_y;
    if (s->shadow) {
        stlxgfx_blit_alpha(back, ox + 1, oy + 1, s->shadow, 0, 0,
                           s->shadow->width, s->shadow->height);
    }
    stlxgfx_blit_alpha(back, ox, oy, s->image, 0, 0,
                       s->image->width, s->image->height);
}
