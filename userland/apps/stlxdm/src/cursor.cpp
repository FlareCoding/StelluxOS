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

/* A two-headed arrow along the main diagonal, mirrored horizontally
 * for the opposite slope */
static stlxgfx_surface_t* build_resize_diag(bool nwse) {
    constexpr int32_t N = 13;
    stlxgfx_surface_t* s = stlxgfx_create_surface(N, N, 32, 16, 8, 0);
    if (!s) {
        return nullptr;
    }

    stlxgfx_clear(s, 0x00000000);
    for (int32_t y = 0; y < N; y++) {
        for (int32_t x = 0; x < N; x++) {
            bool stem = x == y || x == y + 1 || x + 1 == y;
            bool head_a = x + y <= 5;
            bool head_b = x + y >= 2 * (N - 1) - 5;
            if (stem || head_a || head_b) {
                stlxgfx_fill_rect(s, nwse ? x : N - 1 - x, y, 1, 1, FILL);
            }
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

/* The image over its shadow as one picture, alpha not multiplied
 * into the colors, matching `draw()` on a frame within rounding */
static stlxgfx_surface_t* build_picture(const cursor_sprite& sprite) {
    const stlxgfx_surface_t* image = sprite.image;
    const stlxgfx_surface_t* shadow = sprite.shadow;
    bool picture_layout = image->bpp == 32 && image->red_shift == 16 &&
                          image->green_shift == 8 && image->blue_shift == 0;
    if (!picture_layout) {
        return nullptr;
    }

    uint32_t pad = shadow ? 1 : 0;
    stlxgfx_surface_t* picture = stlxgfx_create_surface(
        image->width + pad, image->height + pad, 32, 16, 8, 0);
    if (!picture) {
        return nullptr;
    }

    /* gfxfb takes a cursor picture in rows without padding */
    if (picture->pitch != picture->width * 4) {
        stlxgfx_destroy_surface(picture);
        return nullptr;
    }

    uint8_t alpha_index = stlxgfx_alpha_byte_index(picture);
    for (uint32_t y = 0; y < picture->height; y++) {
        for (uint32_t x = 0; x < picture->width; x++) {
            bool inside_image = x < image->width && y < image->height;
            const uint8_t* ip = nullptr;
            uint32_t image_alpha = 0;
            if (inside_image) {
                ip = image->pixels + y * image->pitch + x * 4;
                image_alpha = ip[alpha_index];
            }

            bool under_shadow = shadow && x >= 1 && y >= 1;
            uint32_t shadow_alpha = 0;
            if (under_shadow) {
                uint32_t offset = (y - 1) * shadow->pitch + (x - 1) * 4;
                shadow_alpha = shadow->pixels[offset + alpha_index];
            }

            /* The black shadow beneath adds coverage, no color */
            uint32_t shadow_cover =
                (shadow_alpha * (255 - image_alpha) + 127) / 255;
            uint32_t alpha = image_alpha + shadow_cover;
            uint8_t* pp = picture->pixels + y * picture->pitch + x * 4;
            for (uint32_t c = 0; c < 4; c++) {
                uint32_t value = 0;
                if (c == alpha_index) {
                    value = alpha;
                } else if (image_alpha > 0) {
                    value = (ip[c] * image_alpha + alpha / 2) / alpha;
                }

                pp[c] = static_cast<uint8_t>(value);
            }
        }
    }

    return picture;
}

static void destroy_sprite(cursor_sprite* s) {
    stlxgfx_destroy_surface(s->image);
    stlxgfx_destroy_surface(s->shadow);
    stlxgfx_destroy_surface(s->picture);
    s->image = nullptr;
    s->shadow = nullptr;
    s->picture = nullptr;
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

    m_resize_nwse.image = build_resize_diag(true);
    m_resize_nwse.hot_x = 6;
    m_resize_nwse.hot_y = 6;

    m_resize_nesw.image = build_resize_diag(false);
    m_resize_nesw.hot_x = 6;
    m_resize_nesw.hot_y = 6;

    cursor_sprite* all[] = { &m_arrow, &m_ibeam, &m_resize_h,
                             &m_resize_v, &m_resize_nwse, &m_resize_nesw };
    for (cursor_sprite* s : all) {
        if (s->image) {
            s->shadow = build_shadow(s->image);
            s->picture = build_picture(*s);
        }
    }

    return m_arrow.image ? 0 : -1;
}

void cursor::shutdown() {
    destroy_sprite(&m_arrow);
    destroy_sprite(&m_ibeam);
    destroy_sprite(&m_resize_h);
    destroy_sprite(&m_resize_v);
    destroy_sprite(&m_resize_nwse);
    destroy_sprite(&m_resize_nesw);
}

const cursor_sprite* cursor::sprite_for(uint32_t shape) const {
    switch (shape) {
    case SWP_CURSOR_IBEAM:       return &m_ibeam;
    case SWP_CURSOR_HAND:        return &m_arrow;
    case SWP_CURSOR_RESIZE_H:    return &m_resize_h;
    case SWP_CURSOR_RESIZE_V:    return &m_resize_v;
    case SWP_CURSOR_RESIZE_NWSE: return &m_resize_nwse;
    case SWP_CURSOR_RESIZE_NESW: return &m_resize_nesw;
    case SWP_CURSOR_NONE:        return nullptr;
    default:                     return &m_arrow;
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
                  int32_t x, int32_t y,
                  const damage_list::rect& clip) const {
    const cursor_sprite* s = sprite_for(shape);
    if (!s || !s->image) {
        return;
    }

    int32_t bx, by, bw, bh;
    bounds(shape, x, y, &bx, &by, &bw, &bh);
    bool left_or_right_of_clip = bx >= clip.x + clip.w || bx + bw <= clip.x;
    bool above_or_below_clip = by >= clip.y + clip.h || by + bh <= clip.y;
    if (left_or_right_of_clip || above_or_below_clip) {
        return;
    }

    /* Pixels outside `clip` may already hold the pointer, from an
     * earlier frame or rect, and a second blend would thicken it */
    stlxgfx_surface_t* view = stlxgfx_surface_from_buffer(
        back->pixels + static_cast<uint32_t>(clip.y) * back->pitch
            + static_cast<uint32_t>(clip.x) * 4,
        static_cast<uint32_t>(clip.w), static_cast<uint32_t>(clip.h),
        back->pitch, 32, 16, 8, 0);
    if (!view) {
        return;
    }

    int32_t ox = bx - clip.x;
    int32_t oy = by - clip.y;
    if (s->shadow) {
        stlxgfx_blit_alpha(view, ox + 1, oy + 1, s->shadow, 0, 0,
                           s->shadow->width, s->shadow->height);
    }

    stlxgfx_blit_alpha(view, ox, oy, s->image, 0, 0,
                       s->image->width, s->image->height);
    stlxgfx_destroy_surface(view);
}

bool cursor::picture(uint32_t shape, presenter::cursor_picture& out) const {
    const cursor_sprite* s = sprite_for(shape);
    if (!s || !s->picture) {
        return false;
    }

    out.pixels = reinterpret_cast<const uint32_t*>(s->picture->pixels);
    out.width = s->picture->width;
    out.height = s->picture->height;
    out.hot_x = static_cast<uint32_t>(s->hot_x);
    out.hot_y = static_cast<uint32_t>(s->hot_y);

    return true;
}
