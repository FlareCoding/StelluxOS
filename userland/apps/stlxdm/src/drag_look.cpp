#include "drag_look.hpp"
#include "animation.hpp"
#include "shell/draw.hpp"

#include <stlxgfx/blend.h>
#include <stlxgfx/font.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <string>

/* The count badge: a pill on the picture's top right corner, at least as wide as it is tall */
constexpr int32_t BADGE_H = 20;
constexpr int32_t BADGE_PAD = 6;
constexpr float BADGE_TEXT_PX = 11.5f;

/* The chip: a body as tall as a toolbar control, holding its glyph and words between these insets */
constexpr int32_t CHIP_H = STLXTHEME_TOOLBAR_CONTROL_HEIGHT;
constexpr int32_t CHIP_GLYPH = STLXTHEME_GLYPH_SIZE;
constexpr int32_t CHIP_PAD_LEFT = 8;
constexpr int32_t CHIP_GAP = 6;
constexpr int32_t CHIP_PAD_RIGHT = 12;
constexpr float CHIP_TEXT_PX = 12.5f;

/* The chip casts an unfocused window's shadow, which reaches its layers' blur from the body, shifted down */
constexpr uint32_t PERCENT = 100;
constexpr const decor::window_shadow_layer& CHIP_CONTACT = decor::UNFOCUSED_SHADOW[0];
constexpr const decor::window_shadow_layer& CHIP_WIDEST = decor::UNFOCUSED_SHADOW[2];
constexpr int32_t CHIP_SHADOW_SIDE = CHIP_WIDEST.blur;
constexpr int32_t CHIP_SHADOW_TOP = CHIP_WIDEST.blur - CHIP_WIDEST.offset_y;
constexpr int32_t CHIP_SHADOW_BOTTOM = CHIP_WIDEST.blur + CHIP_WIDEST.offset_y;

/* A glyph's corners lie on a 16 unit grid around its center */
constexpr float GLYPH_CENTER = 8.0f;
constexpr int32_t CIRCLE_POINTS = 32;
constexpr float RIGHT_ANGLE_DEGREES = 90.0f;
constexpr float HALF_TURN_DEGREES = 180.0f;

static const std::vector<glyph_point> PLUS = {
    { 7, 3 }, { 9, 3 }, { 9, 7 }, { 13, 7 }, { 13, 9 }, { 9, 9 },
    { 9, 13 }, { 7, 13 }, { 7, 9 }, { 3, 9 }, { 3, 7 }, { 7, 7 },
};

static const std::vector<glyph_point> ARROW = {
    { 2.5f, 7 }, { 9, 7 }, { 9, 3.5f }, { 13.5f, 8 }, { 9, 12.5f }, { 9, 9 }, { 2.5f, 9 },
};

/* The refusal's ring, and the slash that crosses its hole without touching the ring */
constexpr float RING_OUTER = 6.5f;
constexpr float RING_INNER = 5.0f;
constexpr float SLASH_HALF_LENGTH = 4.3f;
constexpr float SLASH_HALF_WIDTH = 0.9f;

static glyph_point rotated(glyph_point p, float degrees) {
    float radians = degrees * std::numbers::pi_v<float> / HALF_TURN_DEGREES;
    float dx = p.x - GLYPH_CENTER;
    float dy = p.y - GLYPH_CENTER;

    return { GLYPH_CENTER + dx * std::cos(radians) - dy * std::sin(radians),
             GLYPH_CENTER + dx * std::sin(radians) + dy * std::cos(radians) };
}

static std::vector<glyph_point> rotated(const std::vector<glyph_point>& contour, float degrees) {
    std::vector<glyph_point> turned;
    for (const glyph_point& p : contour) {
        turned.push_back(rotated(p, degrees));
    }

    return turned;
}

static std::vector<glyph_point> circle(float radius) {
    std::vector<glyph_point> points;
    for (int32_t i = 0; i < CIRCLE_POINTS; i++) {
        float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / CIRCLE_POINTS;
        points.push_back({ GLYPH_CENTER + radius * std::cos(angle), GLYPH_CENTER + radius * std::sin(angle) });
    }

    return points;
}

/* Copy is a plus, move an arrow along the drag, link the arrow turned up toward its target, and a refusal a slashed
 * ring, all filled by the evenodd rule */
static std::vector<std::vector<glyph_point>> glyph_contours(dnd_chip::glyph mark) {
    switch (mark) {
    case dnd_chip::glyph::copy:
        return { PLUS };
    case dnd_chip::glyph::move:
        return { ARROW };
    case dnd_chip::glyph::link:
        return { rotated(ARROW, -RIGHT_ANGLE_DEGREES / 2) };
    case dnd_chip::glyph::refusal: {
        std::vector<glyph_point> slash = {
            { GLYPH_CENTER - SLASH_HALF_LENGTH, GLYPH_CENTER - SLASH_HALF_WIDTH },
            { GLYPH_CENTER + SLASH_HALF_LENGTH, GLYPH_CENTER - SLASH_HALF_WIDTH },
            { GLYPH_CENTER + SLASH_HALF_LENGTH, GLYPH_CENTER + SLASH_HALF_WIDTH },
            { GLYPH_CENTER - SLASH_HALF_LENGTH, GLYPH_CENTER + SLASH_HALF_WIDTH },
        };

        return { circle(RING_OUTER), circle(RING_INNER), rotated(slash, RIGHT_ANGLE_DEGREES / 2) };
    }
    case dnd_chip::glyph::none:
        break;
    }

    return {};
}

static uint32_t* row_of(const twinned_picture& picture, int32_t y) {
    return reinterpret_cast<uint32_t*>(picture.pixels() + static_cast<size_t>(y) * picture.pitch());
}

static picture_region region_of(twinned_picture& picture) {
    return { picture.pixels(), picture.address(), picture.pitch(), static_cast<int32_t>(picture.width()),
             static_cast<int32_t>(picture.height()), &picture };
}

/* A surface over the picture's pixels that blends in straight alpha, which the caller destroys */
static stlxgfx_surface_t* surface_over(const twinned_picture& picture) {
    stlxgfx_surface_t* surface = stlxgfx_surface_from_buffer(picture.pixels(), picture.width(), picture.height(),
                                                             picture.pitch(), 32, 16, 8, 0);
    if (surface) {
        surface->flags |= STLXGFX_SURFACE_STRAIGHT_ALPHA;
    }

    return surface;
}

/* `color` at `coverage` out of 255 over the straight alpha `below` */
static uint32_t blend_coverage(uint32_t below, uint32_t color, uint32_t coverage) {
    uint32_t alpha = stlxgfx_div255((color >> decor::PIXEL_ALPHA_SHIFT) * coverage);
    return alpha == 0 ? below : stlxgfx_blend_straight(below, (color & 0x00FFFFFFu) | alpha << decor::PIXEL_ALPHA_SHIFT);
}

/* The straight alpha `source` over the premultiplied `below` */
static uint32_t blend_onto_premultiplied(uint32_t below, uint32_t source) {
    uint32_t alpha = source >> decor::PIXEL_ALPHA_SHIFT;
    uint32_t inverse = decor::CHANNEL_MAX - alpha;
    uint32_t blended = (alpha + stlxgfx_div255((below >> decor::PIXEL_ALPHA_SHIFT) * inverse)) << decor::PIXEL_ALPHA_SHIFT;
    for (uint32_t shift = 0; shift < decor::PIXEL_ALPHA_SHIFT; shift += decor::CHANNEL_BITS) {
        uint32_t channel = stlxgfx_div255(((source >> shift) & decor::CHANNEL_MAX) * alpha) +
                           stlxgfx_div255(((below >> shift) & decor::CHANNEL_MAX) * inverse);
        blended |= std::min(channel, decor::CHANNEL_MAX) << shift;
    }

    return blended;
}

/* The premultiplied `source` faded to `opacity` over the premultiplied `below` */
static uint32_t blend_faded_onto_premultiplied(uint32_t below, uint32_t source, uint32_t opacity) {
    uint32_t alpha = stlxgfx_div255((source >> decor::PIXEL_ALPHA_SHIFT) * opacity);
    uint32_t inverse = decor::CHANNEL_MAX - alpha;
    uint32_t blended = (alpha + stlxgfx_div255((below >> decor::PIXEL_ALPHA_SHIFT) * inverse)) << decor::PIXEL_ALPHA_SHIFT;
    for (uint32_t shift = 0; shift < decor::PIXEL_ALPHA_SHIFT; shift += decor::CHANNEL_BITS) {
        uint32_t channel = stlxgfx_div255(((source >> shift) & decor::CHANNEL_MAX) * opacity) +
                           stlxgfx_div255(((below >> shift) & decor::CHANNEL_MAX) * inverse);
        blended |= std::min(channel, decor::CHANNEL_MAX) << shift;
    }

    return blended;
}

/* A premultiplied pixel in straight alpha, as a hardware cursor takes it */
static uint32_t to_straight(uint32_t pixel) {
    uint32_t alpha = pixel >> decor::PIXEL_ALPHA_SHIFT;
    if (alpha == 0) {
        return 0;
    }

    uint32_t straight = alpha << decor::PIXEL_ALPHA_SHIFT;
    for (uint32_t shift = 0; shift < decor::PIXEL_ALPHA_SHIFT; shift += decor::CHANNEL_BITS) {
        uint32_t channel = (((pixel >> shift) & decor::CHANNEL_MAX) * decor::CHANNEL_MAX + alpha / 2) / alpha;
        straight |= std::min(channel, decor::CHANNEL_MAX) << shift;
    }

    return straight;
}

/* Blends the pixels lying at `at`, rows `stride` pixels apart from `first`, into the premultiplied `pixels` that cover
 * `area`, each through `blend` */
template <typename Blend>
static void blend_into(std::vector<uint32_t>& pixels, const damage_list::rect& area, const uint32_t* first,
                       size_t stride, const damage_list::rect& at, Blend blend) {
    damage_list::rect span = damage_list::intersect(at, area);
    for (int32_t y = span.y; y < span.y + span.h; y++) {
        const uint32_t* source = first + static_cast<size_t>(y - at.y) * stride;
        uint32_t* target = pixels.data() + static_cast<size_t>(y - area.y) * area.w;
        for (int32_t x = span.x; x < span.x + span.w; x++) {
            target[x - area.x] = blend(target[x - area.x], source[x - at.x]);
        }
    }
}

/* The count's pill in straight alpha, which the caller destroys, or null when memory runs out */
static stlxgfx_surface_t* make_badge(uint32_t item_count) {
    std::string count = std::to_string(item_count);
    stlxgfx_font* font = shell_font(STLXTHEME_UI_FONT_SEMIBOLD_PATH, BADGE_TEXT_PX);
    int32_t text_w = font ? stlxgfx_text_width(font, count.data(), count.size()) : 0;
    int32_t width = std::max(BADGE_H, text_w + 2 * BADGE_PAD);

    stlxgfx_surface_t* badge = stlxgfx_create_surface(static_cast<uint32_t>(width), BADGE_H, 32, 16, 8, 0);
    if (!badge) {
        return nullptr;
    }

    badge->flags = STLXGFX_SURFACE_STRAIGHT_ALPHA;
    ui::rect pill = { 0, 0, width, BADGE_H };
    for (int32_t y = 0; y < BADGE_H; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(badge->pixels + static_cast<size_t>(y) * badge->pitch);
        for (int32_t x = 0; x < width; x++) {
            uint32_t coverage = rounded_rect_coverage(x, y, pill, BADGE_H / 2);
            row[x] = (STLXTHEME_TEXT & 0x00FFFFFFu) | coverage << decor::PIXEL_ALPHA_SHIFT;
        }
    }

    if (font) {
        stlxgfx_font_metrics metrics;
        stlxgfx_font_metrics_get(font, &metrics);
        int32_t baseline = (BADGE_H - metrics.line_height) / 2 + metrics.ascent;
        stlxgfx_draw_text(badge, font, (width - text_w) / 2, baseline, count.data(), count.size(), STLXTHEME_WINDOW_BG);
    }

    return badge;
}

/* One pass of the SVG filter spec's box blur along each row or column: every value becomes the mean of the `box`
 * values starting `before` ahead of it */
static void box_pass(std::vector<float>& values, int32_t width, int32_t height, int32_t box, int32_t before,
                     bool is_along_rows) {
    int32_t lines = is_along_rows ? height : width;
    int32_t length = is_along_rows ? width : height;
    int32_t step = is_along_rows ? 1 : width;
    std::vector<float> sums(static_cast<size_t>(length) + 1);
    for (int32_t line = 0; line < lines; line++) {
        float* first = values.data() + (is_along_rows ? static_cast<size_t>(line) * width : static_cast<size_t>(line));
        for (int32_t i = 0; i < length; i++) {
            sums[i + 1] = sums[i] + first[static_cast<size_t>(i) * step];
        }

        for (int32_t i = 0; i < length; i++) {
            int32_t start = std::clamp(i - before, 0, length);
            int32_t end = std::clamp(i - before + box, 0, length);
            first[static_cast<size_t>(i) * step] = (sums[end] - sums[start]) / static_cast<float>(box);
        }
    }
}

/* Three box passes along each axis, the middle one shifted a pixel for an even box and the last a pixel wider, which
 * blurs as decor's window shadows do */
static void blur(std::vector<float>& values, int32_t width, int32_t height, int32_t box) {
    bool is_even = box % 2 == 0;
    int32_t last_box = is_even ? box + 1 : box;
    for (bool is_along_rows : { true, false }) {
        box_pass(values, width, height, box, box / 2, is_along_rows);
        box_pass(values, width, height, box, is_even ? box / 2 - 1 : box / 2, is_along_rows);
        box_pass(values, width, height, last_box, last_box / 2, is_along_rows);
    }
}

/* Spreads each value over the `spread` pixels around it, as a shadow's spread grows its shape */
static void spread_out(std::vector<float>& values, int32_t width, int32_t height, int32_t spread) {
    std::vector<float> spread_rows(values.size());
    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            float most = 0.0f;
            for (int32_t i = std::max(0, x - spread); i <= std::min(width - 1, x + spread); i++) {
                most = std::max(most, values[static_cast<size_t>(y) * width + i]);
            }

            spread_rows[static_cast<size_t>(y) * width + x] = most;
        }
    }

    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            float most = 0.0f;
            for (int32_t j = std::max(0, y - spread); j <= std::min(height - 1, y + spread); j++) {
                most = std::max(most, spread_rows[static_cast<size_t>(j) * width + x]);
            }

            values[static_cast<size_t>(y) * width + x] = most;
        }
    }
}

/* How far a layer's blur reaches past the shape it blurs */
static int32_t blur_reach(const decor::window_shadow_layer& layer) {
    int32_t box = decor::blur_box(layer.blur);
    int32_t last_box = box % 2 == 0 ? box + 1 : box;

    return (2 * box + last_box - 3) / 2 + 1;
}

bool drag_look::draw_picture(picture_maker& maker, const dnd_session& session) {
    m_generation++;
    m_maker = &maker;

    m_picture.reset();
    m_shadow.reset();
    m_chip_picture.reset();
    m_chip = {};
    if (session.picture.empty()) {
        return true;
    }

    /* The badge's center sits on the picture's top right corner, so the picture grows to hold its overhang */
    stlxgfx_surface_t* badge = session.item_count >= 2 ? make_badge(session.item_count) : nullptr;
    int32_t picture_w = static_cast<int32_t>(session.picture_w);
    int32_t picture_h = static_cast<int32_t>(session.picture_h);
    int32_t badge_w = badge ? static_cast<int32_t>(badge->width) : 0;
    int32_t top = badge ? BADGE_H / 2 : 0;

    int32_t left = std::max(0, picture_w - badge_w / 2);
    int32_t width = std::max(picture_w, left + badge_w);
    int32_t height = std::max(picture_h + top, badge ? BADGE_H : 0);
    m_picture = twinned_picture::make(maker, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    if (!m_picture) {
        stlxgfx_destroy_surface(badge);
        return false;
    }

    for (int32_t y = 0; y < height; y++) {
        uint32_t* row = row_of(*m_picture, y);
        std::fill(row, row + width, 0u);
        if (y >= top && y < top + picture_h) {
            const uint32_t* source = session.picture.data() + static_cast<size_t>(y - top) * session.picture_w;
            std::copy(source, source + picture_w, row);
        }
    }

    if (badge) {
        for (int32_t y = 0; y < BADGE_H; y++) {
            const uint32_t* source = reinterpret_cast<const uint32_t*>(badge->pixels + static_cast<size_t>(y) * badge->pitch);
            uint32_t* row = row_of(*m_picture, y);
            for (int32_t x = 0; x < badge_w; x++) {
                row[left + x] = blend_onto_premultiplied(row[left + x], source[x]);
            }
        }

        stlxgfx_destroy_surface(badge);
    }

    m_hot_x = session.hot_x;
    m_hot_y = session.hot_y + top;

    return draw_shadow(maker);
}

/* The ink an unfocused window's shadow layers cast from the picture's coverage, each darkening what the ones before
 * it left clear */
bool drag_look::draw_shadow(picture_maker& maker) {
    int32_t picture_w = static_cast<int32_t>(m_picture->width());
    int32_t picture_h = static_cast<int32_t>(m_picture->height());
    int32_t side = 0;
    int32_t top = 0;
    int32_t bottom = 0;
    for (const decor::window_shadow_layer& layer : decor::UNFOCUSED_SHADOW) {
        int32_t reach = blur_reach(layer) + layer.spread;
        side = std::max(side, reach);
        top = std::max(top, reach - layer.offset_y);
        bottom = std::max(bottom, reach + layer.offset_y);
    }

    int32_t width = picture_w + 2 * side;
    int32_t height = picture_h + top + bottom;
    std::vector<float> clear(static_cast<size_t>(width) * height, 1.0f);
    std::vector<float> covered(clear.size());
    for (const decor::window_shadow_layer& layer : decor::UNFOCUSED_SHADOW) {
        std::fill(covered.begin(), covered.end(), 0.0f);
        for (int32_t y = 0; y < picture_h; y++) {
            const uint32_t* row = row_of(*m_picture, y);
            float* into = covered.data() + static_cast<size_t>(y + top + layer.offset_y) * width + side;
            for (int32_t x = 0; x < picture_w; x++) {
                into[x] = static_cast<float>(row[x] >> decor::PIXEL_ALPHA_SHIFT) / decor::CHANNEL_MAX;
            }
        }

        if (layer.spread > 0) {
            spread_out(covered, width, height, layer.spread);
        }

        if (layer.blur > 0) {
            blur(covered, width, height, decor::blur_box(layer.blur));
        }

        float opacity = static_cast<float>(layer.alpha_percent) / PERCENT;
        for (size_t i = 0; i < clear.size(); i++) {
            clear[i] *= 1.0f - covered[i] * opacity;
        }
    }

    m_shadow = twinned_picture::make(maker, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    if (!m_shadow) {
        m_picture.reset();
        return false;
    }

    for (int32_t y = 0; y < height; y++) {
        uint32_t* row = row_of(*m_shadow, y);
        for (int32_t x = 0; x < width; x++) {
            float ink = 1.0f - clear[static_cast<size_t>(y) * width + x];
            uint32_t alpha = static_cast<uint32_t>(std::lround(std::clamp(ink, 0.0f, 1.0f) * decor::CHANNEL_MAX));
            row[x] = alpha << decor::PIXEL_ALPHA_SHIFT | (STLXTHEME_INK & 0x00FFFFFFu);
        }
    }

    m_shadow_x = -side;
    m_shadow_y = -top;

    return true;
}

bool drag_look::draw_chip(const dnd_chip& chip) {
    m_generation++;
    m_chip = chip;
    m_chip_picture.reset();
    if (chip.mark == dnd_chip::glyph::none || !m_maker) {
        return true;
    }

    stlxgfx_font* font = shell_font(STLXTHEME_UI_FONT_MEDIUM_PATH, CHIP_TEXT_PX);
    int32_t text_w = font ? stlxgfx_text_width(font, chip.words.data(), chip.words.size()) : 0;
    m_chip_body = { CHIP_SHADOW_SIDE, CHIP_SHADOW_TOP, CHIP_PAD_LEFT + CHIP_GLYPH + CHIP_GAP + text_w + CHIP_PAD_RIGHT,
                    CHIP_H };

    int32_t width = m_chip_body.w + 2 * CHIP_SHADOW_SIDE;
    int32_t height = CHIP_H + CHIP_SHADOW_TOP + CHIP_SHADOW_BOTTOM;
    m_chip_picture = twinned_picture::make(*m_maker, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    stlxgfx_surface_t* surface = m_chip_picture ? surface_over(*m_chip_picture) : nullptr;
    if (!surface) {
        m_chip_picture.reset();
        return false;
    }

    shadow_layer layers[std::size(decor::UNFOCUSED_SHADOW) - 1];
    for (size_t i = 1; i < std::size(decor::UNFOCUSED_SHADOW); i++) {
        const decor::window_shadow_layer& layer = decor::UNFOCUSED_SHADOW[i];
        layers[i - 1] = { layer.offset_y, layer.blur, layer.alpha_percent * decor::CHANNEL_MAX / PERCENT };
    }

    ui::rect body = { m_chip_body.x, m_chip_body.y, m_chip_body.w, m_chip_body.h };
    uint32_t contact = static_cast<uint32_t>(CHIP_CONTACT.alpha_percent) * decor::CHANNEL_MAX / PERCENT;
    paint_drop_shadow(surface, body, STLXTHEME_MENU_RADIUS, contact, layers, std::size(layers));

    /* The face, then its rim inside the edge, then the glyph and the words */
    ui::rect inner = { body.x + 1, body.y + 1, body.w - 2, body.h - 2 };
    for (int32_t y = body.y; y < body.y + body.h; y++) {
        uint32_t* row = row_of(*m_chip_picture, y);
        for (int32_t x = body.x; x < body.x + body.w; x++) {
            uint32_t face = rounded_rect_coverage(x, y, body, STLXTHEME_MENU_RADIUS);
            uint32_t within = rounded_rect_coverage(x, y, inner, STLXTHEME_MENU_RADIUS - 1);
            row[x] = blend_coverage(row[x], STLXTHEME_MENU_BG, face);
            row[x] = blend_coverage(row[x], STLXTHEME_RIM, face > within ? face - within : 0);
        }
    }

    bool is_refusal = chip.mark == dnd_chip::glyph::refusal;
    uint32_t glyph_color = is_refusal ? STLXTHEME_TEXT_DIM : STLXTHEME_TEXT;
    stlxgfx_surface_t* glyph = make_shape_glyph(CHIP_GLYPH, glyph_color, glyph_contours(chip.mark));
    int32_t glyph_x = body.x + CHIP_PAD_LEFT;
    int32_t glyph_y = body.y + (CHIP_H - CHIP_GLYPH) / 2;
    for (int32_t y = 0; glyph && y < CHIP_GLYPH; y++) {
        const uint32_t* source = reinterpret_cast<const uint32_t*>(glyph->pixels + static_cast<size_t>(y) * glyph->pitch);
        uint32_t* row = row_of(*m_chip_picture, glyph_y + y);
        for (int32_t x = 0; x < CHIP_GLYPH; x++) {
            if ((source[x] >> decor::PIXEL_ALPHA_SHIFT) != 0) {
                row[glyph_x + x] = stlxgfx_blend_straight(row[glyph_x + x], source[x]);
            }
        }
    }

    stlxgfx_destroy_surface(glyph);
    if (font) {
        stlxgfx_font_metrics metrics;
        stlxgfx_font_metrics_get(font, &metrics);
        int32_t baseline = body.y + (CHIP_H - metrics.line_height) / 2 + metrics.ascent;
        uint32_t text_color = is_refusal ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT;
        stlxgfx_draw_text(surface, font, glyph_x + CHIP_GLYPH + CHIP_GAP, baseline, chip.words.data(),
                          chip.words.size(), text_color);
    }

    stlxgfx_destroy_surface(surface);

    return true;
}

damage_list::rect drag_look::picture_at(int32_t x, int32_t y) const {
    if (!m_picture) {
        return {};
    }

    return { x - m_hot_x, y - m_hot_y, static_cast<int32_t>(m_picture->width()),
             static_cast<int32_t>(m_picture->height()) };
}

/* `length` of the picture's pixels where the picture spans `shown` of its `drawn` pixels */
static int32_t at_scale(int32_t length, int32_t shown, int32_t drawn) {
    return static_cast<int32_t>(static_cast<int64_t>(length) * shown / drawn);
}

damage_list::rect drag_look::shadow_rect(const damage_list::rect& picture) const {
    if (!m_shadow || !m_picture) {
        return {};
    }

    int32_t drawn_w = static_cast<int32_t>(m_picture->width());
    int32_t drawn_h = static_cast<int32_t>(m_picture->height());

    return { picture.x + at_scale(m_shadow_x, picture.w, drawn_w), picture.y + at_scale(m_shadow_y, picture.h, drawn_h),
             at_scale(static_cast<int32_t>(m_shadow->width()), picture.w, drawn_w),
             at_scale(static_cast<int32_t>(m_shadow->height()), picture.h, drawn_h) };
}

/* The chip keeps to the pointer and the picture's place there, wherever a motion shows the picture */
damage_list::rect drag_look::chip_at(const drag_pose& pose) const {
    if (!m_chip_picture || !pose.has_chip) {
        return {};
    }

    damage_list::rect beside = picture_at(pose.pointer_x, pose.pointer_y);
    damage_list::rect body = place_chip(pose.pointer_x, pose.pointer_y, beside, m_chip_body.w, m_chip_body.h,
                                        pose.output);

    return { body.x - m_chip_body.x, body.y - m_chip_body.y, static_cast<int32_t>(m_chip_picture->width()),
             static_cast<int32_t>(m_chip_picture->height()) };
}

damage_list::rect drag_look::bounds(const drag_pose& pose) const {
    damage_list::rect picture = m_picture ? pose.picture : damage_list::rect{};
    damage_list::rect covered = damage_list::union_of(picture, shadow_rect(pose.picture));
    return damage_list::union_of(covered, chip_at(pose));
}

void drag_look::pieces(const drag_pose& pose, std::vector<layer_scaled>& scaled, layer_piece& chip) const {
    if (m_shadow && pose.shadow_opacity > 0) {
        layer_scaled shadow;
        shadow.bounds = shadow_rect(pose.picture);
        shadow.source = region_of(*m_shadow);
        shadow.opacity = pose.shadow_opacity;
        scaled.push_back(shadow);
    }

    if (m_picture && pose.opacity > 0) {
        layer_scaled picture;
        picture.bounds = pose.picture;
        picture.source = region_of(*m_picture);
        picture.opacity = pose.opacity;

        picture.is_content = true;
        picture.is_premultiplied = true;
        scaled.push_back(picture);
    }

    chip = {};
    if (m_chip_picture && pose.has_chip) {
        chip.bounds = chip_at(pose);
        chip.picture = { m_chip_picture->address(), m_chip_picture->pitch(), chip.bounds.x, chip.bounds.y,
                         m_chip_picture.get() };
    }
}

void drag_look::draw(const decor::canvas& canvas, const drag_pose& pose, const damage_list::rect& clip) const {
    std::vector<layer_scaled> scaled;
    layer_piece chip;
    pieces(pose, scaled, chip);
    for (const layer_scaled& piece : scaled) {
        draw_scaled(canvas, piece, clip);
    }

    if (!m_chip_picture || !pose.has_chip) {
        return;
    }

    damage_list::rect area = damage_list::intersect(chip.bounds, clip);
    for (int32_t py = area.y; py < area.y + area.h; py++) {
        const uint32_t* source = row_of(*m_chip_picture, py - chip.bounds.y);
        uint32_t* target = reinterpret_cast<uint32_t*>(canvas.surface->pixels +
                                                       static_cast<size_t>(py - canvas.y) * canvas.surface->pitch);
        for (int32_t px = area.x; px < area.x + area.w; px++) {
            uint32_t pixel = source[px - chip.bounds.x];
            target[px - canvas.x] = decor::blend_over(target[px - canvas.x], pixel, pixel >> decor::PIXEL_ALPHA_SHIFT);
        }
    }
}

static damage_list::rect sprite_rect(const drag_pose& pose, const presenter::cursor_picture& pointer) {
    return { pose.pointer_x - static_cast<int32_t>(pointer.hot_x), pose.pointer_y - static_cast<int32_t>(pointer.hot_y),
             static_cast<int32_t>(pointer.width), static_cast<int32_t>(pointer.height) };
}

damage_list::rect drag_look::cursor_bounds(const drag_pose& pose, const presenter::cursor_picture& pointer) const {
    return damage_list::union_of(bounds(pose), sprite_rect(pose, pointer));
}

/* The parts stack as the frames compose them, the shadow, the picture, the chip and the pointer, over nothing */
void drag_look::cursor_picture(const drag_pose& pose, const presenter::cursor_picture& pointer,
                               std::vector<uint32_t>& pixels, presenter::cursor_picture& out) const {
    damage_list::rect area = cursor_bounds(pose, pointer);
    pixels.assign(static_cast<size_t>(area.w) * static_cast<size_t>(area.h), 0u);
    if (m_shadow && pose.shadow_opacity > 0) {
        auto faded_ink = [&pose](uint32_t below, uint32_t ink) {
            uint32_t alpha = stlxgfx_div255((ink >> decor::PIXEL_ALPHA_SHIFT) * pose.shadow_opacity);
            return blend_onto_premultiplied(below, (ink & 0x00FFFFFFu) | alpha << decor::PIXEL_ALPHA_SHIFT);
        };

        blend_into(pixels, area, row_of(*m_shadow, 0), m_shadow->pitch() / sizeof(uint32_t), shadow_rect(pose.picture),
                   faded_ink);
    }

    if (m_picture && pose.opacity > 0) {
        auto faded = [&pose](uint32_t below, uint32_t pixel) {
            return blend_faded_onto_premultiplied(below, pixel, pose.opacity);
        };

        blend_into(pixels, area, row_of(*m_picture, 0), m_picture->pitch() / sizeof(uint32_t), pose.picture, faded);
    }

    damage_list::rect chip = chip_at(pose);
    if (chip.w > 0 && chip.h > 0) {
        blend_into(pixels, area, row_of(*m_chip_picture, 0), m_chip_picture->pitch() / sizeof(uint32_t), chip,
                   blend_onto_premultiplied);
    }

    blend_into(pixels, area, pointer.pixels, pointer.width, sprite_rect(pose, pointer), blend_onto_premultiplied);
    for (uint32_t& pixel : pixels) {
        pixel = to_straight(pixel);
    }

    out = { pixels.data(), static_cast<uint32_t>(area.w), static_cast<uint32_t>(area.h),
            static_cast<uint32_t>(pose.pointer_x - area.x), static_cast<uint32_t>(pose.pointer_y - area.y) };
}
