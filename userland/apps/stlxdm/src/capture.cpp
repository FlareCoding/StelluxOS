#include "capture.hpp"

#include "material.hpp"
#include "shell/chord.hpp"
#include "shell/draw.hpp"

#include <stlxgfx/font.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

/* The dim over everything but the region, ink at .45 */
constexpr uint32_t DIM_COLOR = 0x73000000u | (STLXTHEME_INK & 0x00FFFFFFu);

/* The hairline lies just outside the region, so it covers no pixel the capture takes */
constexpr int32_t OUTLINE_WIDTH = 1;

/* The tag down and right of the crosshair */
constexpr int32_t TAG_HEIGHT = 22;
constexpr int32_t TAG_PADDING_X = 8;
constexpr int32_t TAG_RADIUS = STLXTHEME_CONTROL_RADIUS;
constexpr int32_t TAG_OFFSET = 16;
constexpr float TAG_TEXT_SIZE = 11.0f;

/* A drag shorter than this either way is a click, and the pointer moves this far with Shift before an axis is kept */
constexpr int32_t MIN_DRAG = 4;
constexpr int32_t SHIFT_SETTLE = 4;

static bool is_empty(const damage_list::rect& r) {
    return r.w <= 0 || r.h <= 0;
}

static bool is_same(const damage_list::rect& a, const damage_list::rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static bool contains(const damage_list::rect& r, int32_t x, int32_t y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static damage_list::rect grown(const damage_list::rect& r, int32_t by) {
    return { r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by };
}

/* Blends `color` over the rounded rect `r` of `view`, its corners antialiased */
static void blend_rounded(stlxgfx_surface_t* view, const damage_list::rect& r, int32_t radius, uint32_t color) {
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        bool is_corner_row = y < r.y + radius || y >= r.y + r.h - radius;
        if (!is_corner_row) {
            stlxgfx_fill_rect_blend(view, r.x, y, static_cast<uint32_t>(r.w), 1, color);
            continue;
        }

        for (int32_t x = r.x; x < r.x + r.w; x++) {
            uint32_t coverage = rounded_rect_coverage(x, y, { r.x, r.y, r.w, r.h }, radius);
            stlxgfx_blend_coverage(view, x, y, color, static_cast<uint8_t>(coverage));
        }
    }
}

/* Glass in a hairline at `box`, in its output's pixels, drawn into `view`, whose first pixel shows the output pixel
 * (`x`, `y`), from the output's `frost`, or on the menu's ink without one */
static void draw_chip_glass(stlxgfx_surface_t* view, int32_t x, int32_t y, const stlxgfx_surface_t* frost,
                            const damage_list::rect& box, int32_t radius) {
    damage_list::rect local = { box.x - x, box.y - y, box.w, box.h };
    int32_t inner_radius = std::max(radius - 1, 0);
    blend_rounded(view, local, radius, material::HAIRLINE);
    if (!frost) {
        blend_rounded(view, grown(local, -1), inner_radius, STLXTHEME_MENU_BG | 0xFF000000u);
        return;
    }

    damage_list::rect area = { x, y, static_cast<int32_t>(view->width), static_cast<int32_t>(view->height) };
    material::draw(view, x, y, frost, { grown(box, -1), inner_radius }, area);
}

stlxgfx_surface_t* copy_frozen_rect(const frozen_frame& frame, const damage_list::rect& r) {
    stlxgfx_surface_t* copy = stlxgfx_create_surface(static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h), 32, 16, 8,
                                                     0);
    if (!copy) {
        return nullptr;
    }

    const stlxgfx_surface_t* from = frame.surface;
    for (int32_t y = 0; y < r.h; y++) {
        const uint8_t* src = from->pixels + static_cast<size_t>(r.y + y) * from->pitch + static_cast<size_t>(r.x) * 4;
        memcpy(copy->pixels + static_cast<size_t>(y) * copy->pitch, src, static_cast<size_t>(r.w) * 4);
    }

    return copy;
}

void capture_overlay::open(std::vector<frozen_frame> frames, int32_t x, int32_t y) {
    m_frames = std::move(frames);
    m_is_open = true;
    m_pointer_x = x;
    m_pointer_y = y;

    m_is_dragging = false;
    m_region = {};
    m_is_space_held = false;
    m_is_shift_held = false;
    m_kept_axis = axis::none;

    m_shown_outline = {};
    m_shown_chip = {};
    m_shown_words.clear();
}

void capture_overlay::close() {
    m_is_open = false;
    m_is_dragging = false;
    m_frames.clear();
}

void capture_overlay::on_motion(int32_t x, int32_t y, uint8_t mods, damage_list& damage) {
    if (m_is_dragging && m_is_space_held) {
        move_region(x, y);
    } else if (m_is_dragging) {
        follow(x, y, mods);
    }

    m_pointer_x = x;
    m_pointer_y = y;
    damage_changes(damage);
}

void capture_overlay::on_press(int32_t x, int32_t y, damage_list& damage) {
    for (uint32_t i = 0; i < m_frames.size(); i++) {
        const damage_list::rect& place = m_frames[i].place;
        bool is_inside = x >= place.x && x < place.x + place.w && y >= place.y && y < place.y + place.h;
        if (m_frames[i].surface && is_inside) {
            m_is_dragging = true;
            m_output = i;
            m_anchor_x = x;
            m_anchor_y = y;
            m_region = { x, y, 0, 0 };

            m_is_shift_held = false;
            m_kept_axis = axis::none;
            break;
        }
    }

    damage_changes(damage);
}

bool capture_overlay::on_release(capture_target* target, damage_list& damage) {
    if (!m_is_dragging) {
        return false;
    }

    bool is_drag = std::max(m_region.w, m_region.h) >= MIN_DRAG && std::min(m_region.w, m_region.h) >= 1;
    if (is_drag) {
        const damage_list::rect& place = m_frames[m_output].place;
        target->output = m_output;
        target->rect = { m_region.x - place.x, m_region.y - place.y, m_region.w, m_region.h };
    }

    m_is_dragging = false;
    m_region = {};
    damage_changes(damage);

    return is_drag;
}

void capture_overlay::hold_space(bool is_held) {
    m_is_space_held = is_held;
    m_move_x = m_pointer_x;
    m_move_y = m_pointer_y;
}

/* The region spans the anchor and the pointer, held to the drag's output, or twice that from the anchor with Alt */
void capture_overlay::follow(int32_t x, int32_t y, uint8_t mods) {
    const damage_list::rect& place = m_frames[m_output].place;
    x = std::clamp(x, place.x, place.x + place.w);
    y = std::clamp(y, place.y, place.y + place.h);

    int32_t dx = x - m_anchor_x;
    int32_t dy = y - m_anchor_y;
    damage_list::rect region = { std::min(m_anchor_x, x), std::min(m_anchor_y, y), std::abs(dx), std::abs(dy) };
    if (mods & CHORD_MOD_ALT) {
        region = { m_anchor_x - std::abs(dx), m_anchor_y - std::abs(dy), 2 * std::abs(dx), 2 * std::abs(dy) };
    }

    bool is_shift_held = (mods & CHORD_MOD_SHIFT) != 0;
    if (is_shift_held && !m_is_shift_held) {
        m_kept_region = m_region;
        m_shift_x = x;
        m_shift_y = y;
        m_kept_axis = axis::none;
    }

    m_is_shift_held = is_shift_held;
    if (is_shift_held && m_kept_axis == axis::none) {
        int32_t moved_x = std::abs(x - m_shift_x);
        int32_t moved_y = std::abs(y - m_shift_y);
        if (std::max(moved_x, moved_y) >= SHIFT_SETTLE) {
            m_kept_axis = moved_x >= moved_y ? axis::horizontal : axis::vertical;
        }
    }

    if (is_shift_held && m_kept_axis == axis::horizontal) {
        region.y = m_kept_region.y;
        region.h = m_kept_region.h;
    } else if (is_shift_held && m_kept_axis == axis::vertical) {
        region.x = m_kept_region.x;
        region.w = m_kept_region.w;
    } else if (is_shift_held) {
        region = m_kept_region;
    }

    m_region = damage_list::intersect(region, place);
}

/* Space carries the region and its anchor with the pointer, inside the drag's output */
void capture_overlay::move_region(int32_t x, int32_t y) {
    const damage_list::rect& place = m_frames[m_output].place;
    int32_t dx = std::clamp(x - m_move_x, place.x - m_region.x, place.x + place.w - m_region.x - m_region.w);
    int32_t dy = std::clamp(y - m_move_y, place.y - m_region.y, place.y + place.h - m_region.y - m_region.h);

    m_region.x += dx;
    m_region.y += dy;
    m_anchor_x += dx;
    m_anchor_y += dy;

    m_move_x = x;
    m_move_y = y;
}

static std::string size_label(const damage_list::rect& r) {
    return std::to_string(r.w) + " \xC3\x97 " + std::to_string(r.h);
}

/* The served output holding (`x`, `y`), else the first one served */
uint32_t capture_overlay::output_at(int32_t x, int32_t y) const {
    uint32_t first = static_cast<uint32_t>(m_frames.size());
    for (uint32_t i = 0; i < m_frames.size(); i++) {
        if (m_frames[i].surface && contains(m_frames[i].place, x, y)) {
            return i;
        }

        first = m_frames[i].surface && first == m_frames.size() ? i : first;
    }

    return first;
}

/* The output the crosshair's tag lies on: the drag's, or the one under the pointer while aiming */
uint32_t capture_overlay::pointer_output() const {
    return m_is_dragging ? m_output : output_at(m_pointer_x, m_pointer_y);
}

/* The region's size once a drag has one, and before that where the crosshair points on its output */
std::string capture_overlay::tag_text() const {
    if (m_is_dragging && !is_empty(m_region)) {
        return size_label(m_region);
    }

    uint32_t output = pointer_output();
    if (output >= m_frames.size()) {
        return {};
    }

    const damage_list::rect& place = m_frames[output].place;
    return std::to_string(m_pointer_x - place.x) + ", " + std::to_string(m_pointer_y - place.y);
}

/* Down and right of the crosshair, turned to its other side where the output ends */
damage_list::rect capture_overlay::chip_rect() const {
    uint32_t output = pointer_output();
    stlxgfx_font* font = shell_font(STLXTHEME_MONO_FONT_PATH, TAG_TEXT_SIZE);
    if (output >= m_frames.size() || !m_frames[output].surface || !font) {
        return {};
    }

    std::string text = tag_text();
    int32_t w = stlxgfx_text_width(font, text.data(), text.size()) + 2 * TAG_PADDING_X;
    int32_t x = m_pointer_x + TAG_OFFSET;
    int32_t y = m_pointer_y + TAG_OFFSET;
    const damage_list::rect& place = m_frames[output].place;
    if (x + w > place.x + place.w) {
        x = m_pointer_x - TAG_OFFSET - w;
    }

    if (y + TAG_HEIGHT > place.y + place.h) {
        y = m_pointer_y - TAG_OFFSET - TAG_HEIGHT;
    }

    x = std::clamp(x, place.x, std::max(place.x, place.x + place.w - w));
    y = std::clamp(y, place.y, std::max(place.y, place.y + place.h - TAG_HEIGHT));
    return { x, y, w, TAG_HEIGHT };
}

/* What the tag says, since its words can change where it stays */
std::string capture_overlay::chip_words() const {
    return tag_text();
}

/* A change repaints where the outline and the chip were and are, and the region's undimmed inside lies in them */
void capture_overlay::damage_changes(damage_list& damage) {
    damage_list::rect outline = m_is_dragging ? grown(m_region, OUTLINE_WIDTH) : damage_list::rect{};
    damage_list::rect chip = chip_rect();
    std::string words = chip_words();
    if (!is_same(outline, m_shown_outline)) {
        for (const damage_list::rect& r : { m_shown_outline, outline }) {
            if (!is_empty(r)) {
                damage.add(r.x, r.y, r.w, r.h);
            }
        }
    }

    if (!is_same(chip, m_shown_chip) || words != m_shown_words) {
        for (const damage_list::rect& r : { m_shown_chip, chip }) {
            if (!is_empty(r)) {
                damage.add(r.x, r.y, r.w, r.h);
            }
        }
    }

    m_shown_outline = outline;
    m_shown_chip = chip;
    m_shown_words = std::move(words);
}

/* The frozen pixels go in opaque, so every later draw blends the same into a scanout buffer and a top picture */
void capture_overlay::draw(uint32_t output, stlxgfx_surface_t* back, const damage_list::rect& clip) const {
    uint8_t* first = back->pixels + static_cast<size_t>(clip.y) * back->pitch + static_cast<size_t>(clip.x) * 4;
    stlxgfx_surface_t* view = stlxgfx_surface_from_buffer(first, static_cast<uint32_t>(clip.w),
                                                          static_cast<uint32_t>(clip.h), back->pitch, 32, 16, 8, 0);
    if (!view) {
        return;
    }

    view->flags = back->flags;
    const stlxgfx_surface_t* frozen = output < m_frames.size() ? m_frames[output].surface : nullptr;
    for (int32_t row = 0; row < clip.h; row++) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(view->pixels + static_cast<size_t>(row) * view->pitch);
        int32_t y = clip.y + row;
        int32_t kept = 0;
        const uint32_t* src = nullptr;
        if (frozen && y < static_cast<int32_t>(frozen->height)) {
            kept = std::clamp(static_cast<int32_t>(frozen->width) - clip.x, 0, clip.w);
            src = reinterpret_cast<const uint32_t*>(frozen->pixels + static_cast<size_t>(y) * frozen->pitch) + clip.x;
        }

        for (int32_t x = 0; x < clip.w; x++) {
            dst[x] = x < kept ? src[x] | 0xFF000000u : STLXTHEME_INK;
        }
    }

    if (!frozen) {
        stlxgfx_destroy_surface(view);
        return;
    }

    /* The view's first pixel on the desktop, and the region on it when the drag lies on this output */
    const damage_list::rect& place = m_frames[output].place;
    int32_t ox = place.x + clip.x;
    int32_t oy = place.y + clip.y;
    bool shows_region = m_is_dragging && m_output == output && !is_empty(m_region);
    damage_list::rect lit = shows_region ? damage_list::rect{ m_region.x - ox, m_region.y - oy, m_region.w,
                                                             m_region.h }
                                         : damage_list::rect{};

    std::vector<damage_list::rect> dimmed = { { 0, 0, clip.w, clip.h } };
    if (shows_region) {
        dimmed = { { 0, 0, clip.w, lit.y },
                   { 0, lit.y + lit.h, clip.w, clip.h - lit.y - lit.h },
                   { 0, lit.y, lit.x, lit.h },
                   { lit.x + lit.w, lit.y, clip.w - lit.x - lit.w, lit.h } };
    }

    for (const damage_list::rect& piece : dimmed) {
        damage_list::rect inside = damage_list::intersect(piece, { 0, 0, clip.w, clip.h });
        if (!is_empty(inside)) {
            stlxgfx_fill_rect_blend(view, inside.x, inside.y, static_cast<uint32_t>(inside.w),
                                    static_cast<uint32_t>(inside.h), DIM_COLOR);
        }
    }

    if (shows_region) {
        damage_list::rect ring = grown(lit, OUTLINE_WIDTH);
        damage_list::rect edges[] = {
            { ring.x, ring.y, ring.w, OUTLINE_WIDTH },
            { ring.x, lit.y + lit.h, ring.w, OUTLINE_WIDTH },
            { ring.x, lit.y, OUTLINE_WIDTH, lit.h },
            { lit.x + lit.w, lit.y, OUTLINE_WIDTH, lit.h },
        };

        for (const damage_list::rect& edge : edges) {
            stlxgfx_fill_rect(view, edge.x, edge.y, static_cast<uint32_t>(edge.w), static_cast<uint32_t>(edge.h),
                              STLXTHEME_ACCENT);
        }
    }

    draw_tag(view, ox, oy, output);
    stlxgfx_destroy_surface(view);
}

/* The tag in the crosshair's numbers, on the glass of the output being drawn */
void capture_overlay::draw_tag(stlxgfx_surface_t* view, int32_t ox, int32_t oy, uint32_t output) const {
    damage_list::rect tag = chip_rect();
    stlxgfx_font* font = shell_font(STLXTHEME_MONO_FONT_PATH, TAG_TEXT_SIZE);
    if (is_empty(tag) || !font) {
        return;
    }

    const damage_list::rect& place = m_frames[output].place;
    const stlxgfx_surface_t* frost = m_frost_of ? m_frost_of(output) : nullptr;
    damage_list::rect on_output = { tag.x - place.x, tag.y - place.y, tag.w, tag.h };
    draw_chip_glass(view, ox - place.x, oy - place.y, frost, on_output, TAG_RADIUS);

    stlxgfx_font_metrics metrics;
    stlxgfx_font_metrics_get(font, &metrics);
    std::string text = tag_text();
    bool shows_size = m_is_dragging && !is_empty(m_region);
    int32_t baseline = tag.y - oy + (tag.h - metrics.ascent - metrics.descent) / 2 + metrics.ascent;
    stlxgfx_draw_text(view, font, tag.x - ox + TAG_PADDING_X, baseline, text.data(), text.size(),
                      shows_size ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY);
}
