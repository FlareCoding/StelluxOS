#include "capture.hpp"

#include "cursor.hpp"
#include "material.hpp"
#include "shell/chord.hpp"
#include "shell/draw.hpp"

#include <stlxgfx/font.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

/* The dim over everything but what a capture takes, ink at .45 */
constexpr uint32_t DIM_COLOR = 0x73000000u | (STLXTHEME_INK & 0x00FFFFFFu);

/* The hairline lies just outside what a capture takes, so it covers none of its pixels */
constexpr int32_t OUTLINE_WIDTH = 1;

/* A chip is a line of words and keycaps on glass, a keycap at either end as far from the edge as from the top */
constexpr int32_t CHIP_HEIGHT = 32;
constexpr int32_t CHIP_RADIUS = STLXTHEME_WINDOW_RADIUS;
constexpr int32_t CHIP_PADDING_X = 14;
constexpr int32_t KEY_HEIGHT = 18;
constexpr int32_t KEY_PADDING_X = 5;
constexpr int32_t KEY_INSET = (CHIP_HEIGHT - KEY_HEIGHT) / 2;
constexpr float KEY_TEXT_SIZE = 11.0f;

/* The tag down and right of the crosshair */
constexpr int32_t TAG_HEIGHT = 22;
constexpr int32_t TAG_PADDING_X = 8;
constexpr int32_t TAG_RADIUS = STLXTHEME_CONTROL_RADIUS;
constexpr int32_t TAG_OFFSET = 16;
constexpr int32_t POINTER_CLEARANCE = 8;
constexpr float TAG_TEXT_SIZE = 11.0f;

/* A drag shorter than this either way is a click, and the pointer moves this far with Shift before an axis is kept */
constexpr int32_t MIN_DRAG = 4;
constexpr int32_t SHIFT_SETTLE = 4;

/* The hint of a mode's keys sits at the top middle of the pointer's output a while */
constexpr uint64_t HINT_SHOWN_NS = 2000000000ull;

/* The preview fits the capture inside a margin above its chip, on a soft drop shadow */
constexpr int32_t PREVIEW_MARGIN = 48;
constexpr int32_t PREVIEW_CHIP_GAP = 24;
constexpr shadow_layer PREVIEW_SHADOW = { 12, 28, 153 };

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

static damage_list::rect moved(const damage_list::rect& r, int32_t dx, int32_t dy) {
    return { r.x + dx, r.y + dy, r.w, r.h };
}

static uint32_t coverage_of(const damage_list::rect& r, int32_t radius, int32_t x, int32_t y) {
    return radius > 0 ? rounded_rect_coverage(x, y, { r.x, r.y, r.w, r.h }, radius) : (contains(r, x, y) ? 255 : 0);
}

static void blend_span(stlxgfx_surface_t* view, int32_t y, int32_t x0, int32_t x1, uint32_t color) {
    x0 = std::max(x0, 0);
    x1 = std::min(x1, static_cast<int32_t>(view->width));
    if (x1 > x0) {
        stlxgfx_fill_rect_blend(view, x0, y, static_cast<uint32_t>(x1 - x0), 1, color);
    }
}

/* Blends `color` over `view` where `outer` covers and `inner` does not, both rounded. Rows clear of the corners blend
 * whole spans, so only corner rows weigh coverage and a large window's ring costs little. */
static void blend_between(stlxgfx_surface_t* view, const damage_list::rect& outer, int32_t outer_radius,
                          const damage_list::rect& inner, int32_t inner_radius, uint32_t color) {
    bool has_inner = !is_empty(inner);
    int32_t corner = std::max(outer_radius, has_inner ? inner.y - outer.y + inner_radius : 0);
    int32_t top = std::max(outer.y, 0);
    int32_t bottom = std::min(outer.y + outer.h, static_cast<int32_t>(view->height));
    for (int32_t y = top; y < bottom; y++) {
        bool is_corner_row = y < outer.y + corner || y >= outer.y + outer.h - corner;
        if (!is_corner_row && !has_inner) {
            blend_span(view, y, outer.x, outer.x + outer.w, color);
            continue;
        }

        if (!is_corner_row) {
            blend_span(view, y, outer.x, inner.x, color);
            blend_span(view, y, inner.x + inner.w, outer.x + outer.w, color);
            continue;
        }

        for (int32_t x = std::max(outer.x, 0); x < std::min(outer.x + outer.w, static_cast<int32_t>(view->width)); x++) {
            uint32_t outside = coverage_of(outer, outer_radius, x, y);
            uint32_t inside = coverage_of(inner, inner_radius, x, y);
            if (outside > inside) {
                stlxgfx_blend_coverage(view, x, y, color, static_cast<uint8_t>(outside - inside));
            }
        }
    }
}

static void blend_rounded(stlxgfx_surface_t* view, const damage_list::rect& r, int32_t radius, uint32_t color) {
    blend_between(view, r, radius, {}, 0, color);
}

/* A one pixel ring just inside the rounded edge of `r` */
static void blend_ring(stlxgfx_surface_t* view, const damage_list::rect& r, int32_t radius, uint32_t color) {
    blend_between(view, r, radius, grown(r, -1), std::max(radius - 1, 0), color);
}

static int32_t piece_width(const chip_piece& piece) {
    stlxgfx_font* font = shell_font(piece.face ? piece.face : STLXTHEME_MONO_FONT_PATH,
                                    piece.face ? piece.size : KEY_TEXT_SIZE);
    int32_t text_w = font ? stlxgfx_text_width(font, piece.text.data(), piece.text.size()) : 0;
    return piece.face ? text_w : std::max(KEY_HEIGHT, text_w + 2 * KEY_PADDING_X);
}

static int32_t line_width(const std::vector<chip_piece>& line) {
    int32_t width = 0;
    for (size_t i = 0; i < line.size(); i++) {
        width += piece_width(line[i]) + (i + 1 < line.size() ? line[i].gap : 0);
    }

    return width;
}

/* Draws `line` from `x`, each piece centered in the `height` rows below `top`, keycaps as the shell's */
static void draw_line(stlxgfx_surface_t* view, int32_t x, int32_t top, int32_t height,
                      const std::vector<chip_piece>& line) {
    for (const chip_piece& piece : line) {
        stlxgfx_font* font = shell_font(piece.face ? piece.face : STLXTHEME_MONO_FONT_PATH,
                                        piece.face ? piece.size : KEY_TEXT_SIZE);
        int32_t width = piece_width(piece);
        if (!font) {
            continue;
        }

        stlxgfx_font_metrics metrics;
        stlxgfx_font_metrics_get(font, &metrics);
        int32_t text_x = x;
        if (!piece.face) {
            damage_list::rect key = { x, top + (height - KEY_HEIGHT) / 2, width, KEY_HEIGHT };
            blend_rounded(view, key, STLXTHEME_CHIP_RADIUS, STLXTHEME_SURFACE);
            blend_ring(view, key, STLXTHEME_CHIP_RADIUS, STLXTHEME_LINE);
            text_x = x + (width - stlxgfx_text_width(font, piece.text.data(), piece.text.size())) / 2;
        }

        int32_t baseline = top + (height - metrics.ascent - metrics.descent) / 2 + metrics.ascent;
        uint32_t color = piece.face ? piece.color : STLXTHEME_TEXT_DIM;
        stlxgfx_draw_text(view, font, text_x, baseline, piece.text.data(), piece.text.size(), color);
        x += width + piece.gap;
    }
}

/* How far a chip's line starts from its left edge */
static int32_t chip_left(const std::vector<chip_piece>& line) {
    return !line.empty() && !line.front().face ? KEY_INSET : CHIP_PADDING_X;
}

static std::string size_label(const damage_list::rect& r) {
    return std::to_string(r.w) + " \xC3\x97 " + std::to_string(r.h);
}

/* What a mode does and its keys, as the palette's footer names its actions */
static std::vector<chip_piece> hint_line(bool is_window_mode) {
    const char* face = STLXTHEME_UI_FONT_MEDIUM_PATH;
    float size = STLXTHEME_CAPTION_SIZE;
    uint32_t action = STLXTHEME_TEXT_SECONDARY;
    if (is_window_mode) {
        return {
            { "Click a window to capture it", face, size, STLXTHEME_TEXT, CHIP_GROUP_GAP },
            { "Without shadow", face, size, action },
            { "Alt", nullptr, 0, 0, CHIP_GROUP_GAP },
            { "Region", face, size, action },
            { "Space", nullptr, 0, 0, CHIP_GROUP_GAP },
            { "Cancel", face, size, action },
            { "Esc" },
        };
    }

    return {
        { "Drag to select a region", face, size, STLXTHEME_TEXT, CHIP_GROUP_GAP },
        { "Window", face, size, action },
        { "Space", nullptr, 0, 0, CHIP_GROUP_GAP },
        { "Cancel", face, size, action },
        { "Esc" },
    };
}

/* The lit window's name and the size its capture takes, the shadow's included unless Alt leaves it out, as the card
 * names it after, or that no window is open */
static std::vector<chip_piece> window_line(const capture_window* window, bool has_shadow) {
    if (!window) {
        return { { "No windows to capture", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT } };
    }

    bool takes_shadow = has_shadow && (window->bounds.w != window->frame.w || window->bounds.h != window->frame.h);
    std::string size = takes_shadow ? size_label(window->bounds) + " with shadow" : size_label(window->frame);
    return {
        { window->title.empty() ? "Window" : window->title, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_CAPTION_SIZE,
          STLXTHEME_TEXT },
        { size, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM },
    };
}

/* A view over `clip` of `back` carrying its flags, which the caller destroys, or null */
static stlxgfx_surface_t* view_of(stlxgfx_surface_t* back, const damage_list::rect& clip) {
    uint8_t* first = back->pixels + static_cast<size_t>(clip.y) * back->pitch + static_cast<size_t>(clip.x) * 4;
    stlxgfx_surface_t* view = stlxgfx_surface_from_buffer(first, static_cast<uint32_t>(clip.w),
                                                          static_cast<uint32_t>(clip.h), back->pitch, 32, 16, 8, 0);
    if (view) {
        view->flags = back->flags;
    }

    return view;
}

/* Copies the frozen pixels under `clip` into `view` opaque, ink past the frame's edge */
static void copy_opaque(stlxgfx_surface_t* view, const stlxgfx_surface_t* frozen, const damage_list::rect& clip) {
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
}

void draw_chip_glass(stlxgfx_surface_t* target, int32_t x, int32_t y, const stlxgfx_surface_t* frost,
                     const damage_list::rect& box, int32_t radius) {
    damage_list::rect local = moved(box, -x, -y);
    int32_t inner_radius = std::max(radius - 1, 0);
    blend_rounded(target, local, radius, material::HAIRLINE);
    if (!frost) {
        blend_rounded(target, grown(local, -1), inner_radius, STLXTHEME_MENU_BG | 0xFF000000u);
        return;
    }

    damage_list::rect area = { x, y, static_cast<int32_t>(target->width), static_cast<int32_t>(target->height) };
    material::draw(target, x, y, frost, { grown(box, -1), inner_radius }, area);
}

int32_t chip_width(const std::vector<chip_piece>& line) {
    if (line.empty()) {
        return 0;
    }

    int32_t right = line.back().face ? CHIP_PADDING_X : KEY_INSET;
    return chip_left(line) + line_width(line) + right;
}

stlxgfx_surface_t* make_chip_picture(const std::vector<chip_piece>& line, const stlxgfx_surface_t* frost, int32_t x,
                                     int32_t y) {
    int32_t w = chip_width(line);
    stlxgfx_surface_t* chip = w > 0 ? stlxgfx_create_surface(static_cast<uint32_t>(w), CHIP_HEIGHT, 32, 16, 8, 0)
                                    : nullptr;
    if (!chip) {
        return nullptr;
    }

    chip->flags = STLXGFX_SURFACE_STRAIGHT_ALPHA;
    stlxgfx_clear(chip, 0);
    draw_chip_glass(chip, x, y, frost, { x, y, w, CHIP_HEIGHT }, CHIP_RADIUS);
    draw_line(chip, chip_left(line), 0, CHIP_HEIGHT, line);
    return chip;
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

capture_overlay::~capture_overlay() {
    stlxgfx_destroy_surface(m_hint);
}

void capture_overlay::open(std::vector<frozen_frame> frames, std::vector<capture_window> windows, int32_t x,
                           int32_t y, uint64_t now_ns, int32_t bar_output) {
    m_frames = std::move(frames);
    m_windows = std::move(windows);
    m_is_open = true;
    m_mode = capture_mode::region;
    m_pointer_x = x;
    m_pointer_y = y;

    m_is_dragging = false;
    m_region = {};
    m_has_region = false;
    m_is_space_held = false;
    m_is_shift_held = false;
    m_kept_axis = axis::none;

    m_lit = -1;
    m_pressed = -1;
    m_lit_output = output_at(x, y);
    m_pressed_output = UINT32_MAX;
    m_bar_pressed = capture_bar::control::none;

    m_shown_outline = {};
    m_shown_chip = {};
    m_shown_words.clear();

    size_t bar_index = static_cast<size_t>(bar_output);
    if (bar_output >= 0 && bar_index < m_frames.size() && m_frames[bar_index].surface) {
        const stlxgfx_surface_t* frost = m_frost_of ? m_frost_of(static_cast<uint32_t>(bar_output)) : nullptr;
        m_bar.show(m_frames[bar_index].place, frost, capture_mode::region);
        return;
    }

    place_hint(now_ns);
}

void capture_overlay::close() {
    m_is_open = false;
    m_is_dragging = false;
    m_has_region = false;
    m_bar.hide();

    m_frames.clear();
    m_windows.clear();
    stlxgfx_destroy_surface(m_hint);
    m_hint = nullptr;
}

int64_t capture_overlay::timeout_ns(uint64_t now_ns) const {
    if (!m_hint) {
        return -1;
    }

    return m_hint_until_ns > now_ns ? static_cast<int64_t>(m_hint_until_ns - now_ns) : 0;
}

void capture_overlay::tick(uint64_t now_ns, damage_list& damage) {
    if (m_hint && now_ns >= m_hint_until_ns) {
        hide_hint(damage);
    }
}

/* The hint of the mode's keys at the top middle of the pointer's output */
void capture_overlay::place_hint(uint64_t now_ns) {
    stlxgfx_destroy_surface(m_hint);
    m_hint = nullptr;
    uint32_t output = output_at(m_pointer_x, m_pointer_y);
    if (output >= m_frames.size()) {
        return;
    }

    std::vector<chip_piece> line = hint_line(m_mode == capture_mode::window);
    const damage_list::rect& place = m_frames[output].place;
    int32_t x = (place.w - chip_width(line)) / 2;
    m_hint = make_chip_picture(line, m_frost_of ? m_frost_of(output) : nullptr, x, CAPTURE_HINT_TOP);
    if (m_hint) {
        m_hint_rect = { place.x + x, place.y + CAPTURE_HINT_TOP, static_cast<int32_t>(m_hint->width),
                        static_cast<int32_t>(m_hint->height) };
        m_hint_until_ns = now_ns + HINT_SHOWN_NS;
    }
}

void capture_overlay::hide_hint(damage_list& damage) {
    if (m_hint) {
        damage.add(m_hint_rect.x, m_hint_rect.y, m_hint_rect.w, m_hint_rect.h);
        stlxgfx_destroy_surface(m_hint);
        m_hint = nullptr;
    }
}

void capture_overlay::on_motion(int32_t x, int32_t y, uint8_t mods, damage_list& damage) {
    m_mods = mods;
    m_bar.light(m_bar.control_at(x, y), damage);
    bool is_over_bar = is_on_bar(x, y);
    if (m_mode == capture_mode::window && !is_over_bar) {
        m_lit = window_at(x, y);
    } else if (m_mode == capture_mode::screen && !is_over_bar) {
        m_lit_output = output_at(x, y);
    } else if (m_is_dragging && m_is_space_held) {
        move_region(x, y);
    } else if (m_is_dragging) {
        follow(x, y, mods);
    }

    m_pointer_x = x;
    m_pointer_y = y;
    damage_changes(damage);
}

void capture_overlay::on_press(int32_t x, int32_t y, uint8_t mods, damage_list& damage) {
    if (is_on_bar(x, y)) {
        m_bar_pressed = m_bar.control_at(x, y);
        return;
    }

    if (m_mode == capture_mode::window) {
        m_pressed = m_lit;
        m_press_has_shadow = (mods & CHORD_MOD_ALT) == 0;
        return;
    }

    if (m_mode == capture_mode::screen) {
        m_pressed_output = m_lit_output;
        return;
    }

    uint32_t output = output_at(x, y);
    if (output < m_frames.size() && m_frames[output].surface && contains(m_frames[output].place, x, y)) {
        m_previous_output = m_output;
        m_previous_region = m_has_region ? m_region : damage_list::rect{};
        m_has_region = false;

        m_is_dragging = true;
        m_output = output;
        m_anchor_x = x;
        m_anchor_y = y;
        m_region = { x, y, 0, 0 };

        m_is_shift_held = false;
        m_kept_axis = axis::none;
    }

    damage_changes(damage);
}

capture_result capture_overlay::on_release(capture_target* target, damage_list& damage) {
    if (m_bar_pressed != capture_bar::control::none) {
        capture_bar::control pressed = m_bar_pressed;
        m_bar_pressed = capture_bar::control::none;
        bool is_click = m_bar.control_at(m_pointer_x, m_pointer_y) == pressed;
        return is_click ? press_control(pressed, target, damage) : capture_result::none;
    }

    if (m_mode == capture_mode::window) {
        bool is_click = m_pressed >= 0 && m_pressed == m_lit;
        if (is_click) {
            target->window = m_windows[static_cast<size_t>(m_lit)].id;
            target->has_shadow = m_press_has_shadow;
        }

        m_pressed = -1;
        return is_click ? capture_result::take : capture_result::none;
    }

    if (m_mode == capture_mode::screen) {
        bool is_click = m_pressed_output != UINT32_MAX && m_pressed_output == m_lit_output;
        m_pressed_output = UINT32_MAX;
        return is_click && take(target) ? capture_result::take : capture_result::none;
    }

    if (!m_is_dragging) {
        return capture_result::none;
    }

    m_is_dragging = false;
    bool is_drag = std::max(m_region.w, m_region.h) >= MIN_DRAG && std::min(m_region.w, m_region.h) >= 1;
    if (m_bar.shown()) {
        m_has_region = is_drag || !is_empty(m_previous_region);
        m_output = is_drag ? m_output : m_previous_output;
        m_region = is_drag ? m_region : m_previous_region;
        damage_changes(damage);
        return capture_result::none;
    }

    if (is_drag) {
        const damage_list::rect& place = m_frames[m_output].place;
        target->output = m_output;
        target->rect = { m_region.x - place.x, m_region.y - place.y, m_region.w, m_region.h };
    }

    m_region = {};
    damage_changes(damage);

    return is_drag ? capture_result::take : capture_result::none;
}

/* A mode chooses its targets, Capture takes what they light, and close ends the overlay */
capture_result capture_overlay::press_control(capture_bar::control control, capture_target* target,
                                              damage_list& damage) {
    switch (control) {
    case capture_bar::control::region:
        choose(capture_mode::region, damage);
        return capture_result::none;
    case capture_bar::control::window:
        choose(capture_mode::window, damage);
        return capture_result::none;
    case capture_bar::control::screen:
        choose(capture_mode::screen, damage);
        return capture_result::none;
    case capture_bar::control::capture:
        return take(target) ? capture_result::take : capture_result::none;
    case capture_bar::control::close:
        return capture_result::close;
    case capture_bar::control::none:
        break;
    }

    return capture_result::none;
}

void capture_overlay::choose(capture_mode mode, damage_list& damage) {
    if (mode == m_mode) {
        return;
    }

    m_mode = mode;
    m_lit = mode == capture_mode::window ? window_at(m_pointer_x, m_pointer_y) : -1;
    m_lit_output = output_at(m_pointer_x, m_pointer_y);
    m_pressed = -1;
    m_pressed_output = UINT32_MAX;

    m_bar.choose(mode, damage);
    damage_everything(damage);
    damage_changes(damage);
}

bool capture_overlay::take(capture_target* target) const {
    if (m_mode == capture_mode::window) {
        if (m_lit < 0) {
            return false;
        }

        target->window = m_windows[static_cast<size_t>(m_lit)].id;
        target->has_shadow = (m_mods & CHORD_MOD_ALT) == 0;
        return true;
    }

    if (m_mode == capture_mode::screen) {
        if (m_lit_output >= m_frames.size() || !m_frames[m_lit_output].surface) {
            return false;
        }

        const damage_list::rect& place = m_frames[m_lit_output].place;
        target->output = m_lit_output;
        target->rect = { 0, 0, place.w, place.h };
        target->kind = capture_kind::screen;
        return true;
    }

    if (!m_has_region || is_empty(m_region)) {
        return false;
    }

    const damage_list::rect& place = m_frames[m_output].place;
    target->output = m_output;
    target->rect = { m_region.x - place.x, m_region.y - place.y, m_region.w, m_region.h };
    return true;
}

bool capture_overlay::is_on_bar(int32_t x, int32_t y) const {
    return m_bar.shown() && contains(m_bar.rect(), x, y);
}

void capture_overlay::press_space(uint64_t now_ns, damage_list& damage) {
    if (m_mode == capture_mode::region && m_is_dragging) {
        m_is_space_held = true;
        m_move_x = m_pointer_x;
        m_move_y = m_pointer_y;
        return;
    }

    choose(m_mode == capture_mode::region ? capture_mode::window : capture_mode::region, damage);
    if (!m_bar.shown()) {
        hide_hint(damage);
        place_hint(now_ns);
    }
}

void capture_overlay::release_space() {
    m_is_space_held = false;
}

uint32_t capture_overlay::cursor_shape() const {
    if (is_on_bar(m_pointer_x, m_pointer_y)) {
        return SWP_CURSOR_ARROW;
    }

    return m_mode == capture_mode::region ? CURSOR_CROSSHAIR : CURSOR_CAMERA;
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

int32_t capture_overlay::window_at(int32_t x, int32_t y) const {
    for (size_t i = 0; i < m_windows.size(); i++) {
        if (contains(m_windows[i].frame, x, y)) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
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

/* The output the crosshair's marks lie on: the drag's, or the one under the pointer while aiming */
uint32_t capture_overlay::pointer_output() const {
    return m_is_dragging ? m_output : output_at(m_pointer_x, m_pointer_y);
}

/* The region's size once a drag or the bar has one, and before that where the crosshair points on its output */
std::string capture_overlay::tag_text() const {
    if ((m_is_dragging || m_has_region) && !is_empty(m_region)) {
        return size_label(m_region);
    }

    uint32_t output = pointer_output();
    if (output >= m_frames.size()) {
        return {};
    }

    const damage_list::rect& place = m_frames[output].place;
    return std::to_string(m_pointer_x - place.x) + ", " + std::to_string(m_pointer_y - place.y);
}

/* The tag beside the crosshair, turned to its other side where the output ends, or the window's chip at its middle,
 * or the note that no window is open at the middle of the pointer's output */
damage_list::rect capture_overlay::chip_rect() const {
    if (m_mode == capture_mode::screen || is_on_bar(m_pointer_x, m_pointer_y)) {
        return {};
    }

    if (m_mode == capture_mode::window) {
        if (m_lit < 0 && !m_windows.empty()) {
            return {};
        }

        const capture_window* window = m_lit >= 0 ? &m_windows[static_cast<size_t>(m_lit)] : nullptr;
        int32_t w = chip_width(window_line(window, (m_mods & CHORD_MOD_ALT) == 0));
        int32_t h = CHIP_HEIGHT;
        damage_list::rect around = window ? window->frame : damage_list::rect{ m_pointer_x, m_pointer_y, 0, 0 };
        uint32_t output = output_at(around.x + around.w / 2, around.y + around.h / 2);
        if (output >= m_frames.size()) {
            return {};
        }

        const damage_list::rect& place = m_frames[output].place;
        if (!window) {
            around = place;
        }

        int32_t x = std::clamp(around.x + (around.w - w) / 2, place.x, std::max(place.x, place.x + place.w - w));
        int32_t y = std::clamp(around.y + (around.h - h) / 2, place.y, std::max(place.y, place.y + place.h - h));

        /* The camera never covers the chip: under it the chip moves below the pointer, or above it near the window's
         * bottom, staying inside the window */
        damage_list::rect camera = { m_pointer_x - CURSOR_CAMERA_WIDTH / 2, m_pointer_y - CURSOR_CAMERA_HEIGHT / 2,
                                     CURSOR_CAMERA_WIDTH, CURSOR_CAMERA_HEIGHT };
        if (window && !is_empty(damage_list::intersect({ x, y, w, h }, camera))) {
            int32_t below = m_pointer_y + CURSOR_CAMERA_HEIGHT + POINTER_CLEARANCE;
            int32_t above = m_pointer_y - CURSOR_CAMERA_HEIGHT - POINTER_CLEARANCE - h;
            y = below + h <= around.y + around.h ? below : above;
            y = std::clamp(y, place.y, std::max(place.y, place.y + place.h - h));
        }

        return { x, y, w, h };
    }

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

/* What the chip says, since its words can change where it stays */
std::string capture_overlay::chip_words() const {
    if (m_mode == capture_mode::region) {
        return tag_text();
    }

    if (m_mode == capture_mode::screen) {
        return {};
    }

    std::string words;
    const capture_window* window = m_lit >= 0 ? &m_windows[static_cast<size_t>(m_lit)] : nullptr;
    for (const chip_piece& piece : window_line(window, (m_mods & CHORD_MOD_ALT) == 0)) {
        words += piece.text;
    }

    return words;
}

/* A change repaints where the outline and the chip were and are, and what they light lies in them */
void capture_overlay::damage_changes(damage_list& damage) {
    damage_list::rect outline = {};
    if (m_mode == capture_mode::window && m_lit >= 0) {
        outline = m_windows[static_cast<size_t>(m_lit)].frame;
    } else if (m_mode == capture_mode::screen && m_lit_output < m_frames.size()) {
        outline = m_frames[m_lit_output].place;
    } else if (m_mode == capture_mode::region && (m_is_dragging || m_has_region)) {
        outline = grown(m_region, OUTLINE_WIDTH);
    }

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

void capture_overlay::damage_everything(damage_list& damage) const {
    for (const frozen_frame& frame : m_frames) {
        if (frame.surface) {
            damage.add(frame.place.x, frame.place.y, frame.place.w, frame.place.h);
        }
    }
}

/* The frozen pixels go in opaque, so every later draw blends the same into a scanout buffer and a top picture */
void capture_overlay::draw(uint32_t output, stlxgfx_surface_t* back, const damage_list::rect& clip) const {
    stlxgfx_surface_t* view = view_of(back, clip);
    if (!view) {
        return;
    }

    const stlxgfx_surface_t* frozen = output < m_frames.size() ? m_frames[output].surface : nullptr;
    copy_opaque(view, frozen, clip);
    if (frozen) {
        /* The view's first pixel on the desktop */
        int32_t ox = m_frames[output].place.x + clip.x;
        int32_t oy = m_frames[output].place.y + clip.y;
        if (m_mode == capture_mode::window) {
            draw_window(view, ox, oy);
        } else if (m_mode == capture_mode::screen) {
            draw_screen(view, ox, oy, output);
        } else {
            draw_region(view, ox, oy, output);
        }

        draw_chip(view, ox, oy, output);

        if (m_hint) {
            stlxgfx_blit_alpha(view, m_hint_rect.x - ox, m_hint_rect.y - oy, m_hint, 0, 0, m_hint->width,
                               m_hint->height);
        }

        if (m_bar.shown() && !is_empty(damage_list::intersect(m_bar.rect(), m_frames[output].place))) {
            m_bar.draw(view, ox, oy);
        }
    }

    stlxgfx_destroy_surface(view);
}

/* Everything dims but the region of a drag on this output, which a hairline rings */
void capture_overlay::draw_region(stlxgfx_surface_t* view, int32_t ox, int32_t oy, uint32_t output) const {
    int32_t w = static_cast<int32_t>(view->width);
    int32_t h = static_cast<int32_t>(view->height);
    bool shows_region = (m_is_dragging || m_has_region) && m_output == output && !is_empty(m_region);
    if (!shows_region) {
        stlxgfx_fill_rect_blend(view, 0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h), DIM_COLOR);
        return;
    }

    damage_list::rect lit = { m_region.x - ox, m_region.y - oy, m_region.w, m_region.h };
    damage_list::rect dimmed[] = { { 0, 0, w, lit.y },
                                   { 0, lit.y + lit.h, w, h - lit.y - lit.h },
                                   { 0, lit.y, lit.x, lit.h },
                                   { lit.x + lit.w, lit.y, w - lit.x - lit.w, lit.h } };
    for (const damage_list::rect& piece : dimmed) {
        damage_list::rect inside = damage_list::intersect(piece, { 0, 0, w, h });
        if (!is_empty(inside)) {
            stlxgfx_fill_rect_blend(view, inside.x, inside.y, static_cast<uint32_t>(inside.w),
                                    static_cast<uint32_t>(inside.h), DIM_COLOR);
        }
    }

    blend_between(view, grown(lit, OUTLINE_WIDTH), 0, lit, 0, STLXTHEME_ACCENT);
}

/* Everything dims but the lit window, whose corners stay rounded, its edge drawn over as a hairline since the capture
 * takes the window's own pixels */
void capture_overlay::draw_window(stlxgfx_surface_t* view, int32_t ox, int32_t oy) const {
    int32_t w = static_cast<int32_t>(view->width);
    int32_t h = static_cast<int32_t>(view->height);
    if (m_lit < 0) {
        stlxgfx_fill_rect_blend(view, 0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h), DIM_COLOR);
        return;
    }

    const capture_window& window = m_windows[static_cast<size_t>(m_lit)];
    damage_list::rect lit = { window.frame.x - ox, window.frame.y - oy, window.frame.w, window.frame.h };
    damage_list::rect dimmed[] = { { 0, 0, w, lit.y },
                                   { 0, lit.y + lit.h, w, h - lit.y - lit.h },
                                   { 0, lit.y, lit.x, lit.h },
                                   { lit.x + lit.w, lit.y, w - lit.x - lit.w, lit.h } };
    for (const damage_list::rect& piece : dimmed) {
        damage_list::rect inside = damage_list::intersect(piece, { 0, 0, w, h });
        if (!is_empty(inside)) {
            stlxgfx_fill_rect_blend(view, inside.x, inside.y, static_cast<uint32_t>(inside.w),
                                    static_cast<uint32_t>(inside.h), DIM_COLOR);
        }
    }

    /* The corner squares dim outside each rounded corner */
    int32_t r = window.radius;
    damage_list::rect corners[] = { { lit.x, lit.y, r, r },
                                    { lit.x + lit.w - r, lit.y, r, r },
                                    { lit.x, lit.y + lit.h - r, r, r },
                                    { lit.x + lit.w - r, lit.y + lit.h - r, r, r } };
    for (const damage_list::rect& corner : corners) {
        damage_list::rect inside = damage_list::intersect(corner, { 0, 0, w, h });
        for (int32_t y = inside.y; y < inside.y + inside.h; y++) {
            for (int32_t x = inside.x; x < inside.x + inside.w; x++) {
                uint32_t covered = coverage_of(lit, r, x, y);
                stlxgfx_blend_coverage(view, x, y, DIM_COLOR, static_cast<uint8_t>(255 - covered));
            }
        }
    }

    blend_ring(view, lit, r, STLXTHEME_ACCENT);
}

/* Every output dims but the one under the pointer, which an amber hairline rings inside its edge */
void capture_overlay::draw_screen(stlxgfx_surface_t* view, int32_t ox, int32_t oy, uint32_t output) const {
    if (output != m_lit_output) {
        stlxgfx_fill_rect_blend(view, 0, 0, view->width, view->height, DIM_COLOR);
        return;
    }

    const damage_list::rect& place = m_frames[output].place;
    blend_ring(view, { place.x - ox, place.y - oy, place.w, place.h }, 0, STLXTHEME_ACCENT);
}

/* The tag in the crosshair's numbers, or the window's chip, on the glass of the output being drawn */
void capture_overlay::draw_chip(stlxgfx_surface_t* view, int32_t ox, int32_t oy, uint32_t output) const {
    damage_list::rect chip = chip_rect();
    if (is_empty(chip)) {
        return;
    }

    const damage_list::rect& place = m_frames[output].place;
    const stlxgfx_surface_t* frost = m_frost_of ? m_frost_of(output) : nullptr;
    damage_list::rect box = moved(chip, -ox, -oy);
    if (m_mode == capture_mode::region) {
        bool shows_size = (m_is_dragging || m_has_region) && !is_empty(m_region);
        std::vector<chip_piece> line = { { tag_text(), STLXTHEME_MONO_FONT_PATH, TAG_TEXT_SIZE,
                                           shows_size ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY } };
        draw_chip_glass(view, ox - place.x, oy - place.y, frost, moved(chip, -place.x, -place.y), TAG_RADIUS);
        draw_line(view, box.x + TAG_PADDING_X, box.y, box.h, line);
        return;
    }

    const capture_window* window = m_lit >= 0 ? &m_windows[static_cast<size_t>(m_lit)] : nullptr;
    std::vector<chip_piece> line = window_line(window, (m_mods & CHORD_MOD_ALT) == 0);
    draw_chip_glass(view, ox - place.x, oy - place.y, frost, moved(chip, -place.x, -place.y), CHIP_RADIUS);
    draw_line(view, box.x + chip_left(line), box.y, box.h, line);
}

/* The average of the pixels in [left, right) by [top, bottom), their colors weighed by their alpha */
static uint32_t average_of(const stlxgfx_surface_t* pixels, bool keeps_alpha, int64_t left, int64_t right, int64_t top,
                           int64_t bottom) {
    uint64_t sums[3] = {};
    uint64_t alpha_sum = 0;
    for (int64_t y = top; y < bottom; y++) {
        const uint32_t* line = reinterpret_cast<const uint32_t*>(pixels->pixels + static_cast<size_t>(y) * pixels->pitch);
        for (int64_t x = left; x < right; x++) {
            uint32_t alpha = keeps_alpha ? line[x] >> 24 : 255;
            alpha_sum += alpha;
            for (uint32_t c = 0; c < 3; c++) {
                sums[c] += ((line[x] >> (16 - 8 * c)) & 0xFF) * alpha;
            }
        }
    }

    uint64_t count = static_cast<uint64_t>((right - left) * (bottom - top));
    uint32_t pixel = static_cast<uint32_t>((alpha_sum + count / 2) / count) << 24;
    for (uint32_t c = 0; alpha_sum > 0 && c < 3; c++) {
        pixel |= static_cast<uint32_t>((sums[c] + alpha_sum / 2) / alpha_sum) << (16 - 8 * c);
    }

    return pixel;
}

stlxgfx_surface_t* shrink_capture(const stlxgfx_surface_t* pixels, bool keeps_alpha, int32_t w, int32_t h) {
    stlxgfx_surface_t* shrunk = stlxgfx_create_surface(static_cast<uint32_t>(w), static_cast<uint32_t>(h), 32, 16, 8, 0);
    if (!shrunk) {
        return nullptr;
    }

    shrunk->flags = keeps_alpha ? STLXGFX_SURFACE_STRAIGHT_ALPHA : 0;
    int64_t source_w = pixels->width;
    int64_t source_h = pixels->height;
    for (int32_t y = 0; y < h; y++) {
        int64_t top = y * source_h / h;
        int64_t bottom = std::max(top + 1, (y + 1) * source_h / h);
        uint32_t* row = reinterpret_cast<uint32_t*>(shrunk->pixels + static_cast<size_t>(y) * shrunk->pitch);
        for (int32_t x = 0; x < w; x++) {
            int64_t left = x * source_w / w;
            int64_t right = std::max(left + 1, (x + 1) * source_w / w);
            row[x] = average_of(pixels, keeps_alpha, left, right, top, bottom);
        }
    }

    return shrunk;
}

/* A soft drop shadow under `box` on `frame` */
static void draw_shadow(stlxgfx_surface_t* frame, const damage_list::rect& box) {
    int32_t side = PREVIEW_SHADOW.blur;
    int32_t above = PREVIEW_SHADOW.blur - PREVIEW_SHADOW.offset_y;
    int32_t below = PREVIEW_SHADOW.blur + PREVIEW_SHADOW.offset_y;
    stlxgfx_surface_t* shadow = stlxgfx_create_surface(static_cast<uint32_t>(box.w + 2 * side),
                                                       static_cast<uint32_t>(above + box.h + below), 32, 16, 8, 0);
    if (!shadow) {
        return;
    }

    paint_drop_shadow(shadow, { side, above, box.w, box.h }, 0, 0, &PREVIEW_SHADOW, 1);
    stlxgfx_blit_alpha(frame, box.x - side, box.y - above, shadow, 0, 0, shadow->width, shadow->height);
    stlxgfx_destroy_surface(shadow);
}

/* The chip under the capture: its kind and size, where it is now, and the keys that discard it and close the
 * preview */
static void draw_preview_chip(stlxgfx_surface_t* frame, const stlxgfx_surface_t* frost, const preview_source& source,
                              int32_t top) {
    const char* face = STLXTHEME_UI_FONT_MEDIUM_PATH;
    float size = STLXTHEME_CAPTION_SIZE;
    std::vector<chip_piece> line = {
        { source.title, face, size, STLXTHEME_TEXT },
        { source.is_current ? "on the clipboard" : "in the clipboard history", STLXTHEME_UI_FONT_PATH, size,
          STLXTHEME_TEXT_DIM, CHIP_GROUP_GAP },
        { "Discard", face, size, STLXTHEME_TEXT_SECONDARY },
        { "Delete", nullptr, 0, 0, CHIP_GROUP_GAP },
        { "Close", face, size, STLXTHEME_TEXT_SECONDARY },
        { "Esc" },
    };

    int32_t w = chip_width(line);
    damage_list::rect chip = { (static_cast<int32_t>(frame->width) - w) / 2, top, w, CHIP_HEIGHT };
    draw_chip_glass(frame, 0, 0, frost, chip, CHIP_RADIUS);
    draw_line(frame, chip.x + chip_left(line), chip.y, CHIP_HEIGHT, line);
}

/* The capture fitted above its chip, never grown, in a one pixel ring on its shadow, or as it is when it has alpha,
 * since a window's capture carries its own frame and shadow */
static void draw_preview(stlxgfx_surface_t* frame, const stlxgfx_surface_t* frost, const preview_source& source) {
    int32_t room_w = static_cast<int32_t>(frame->width) - 2 * PREVIEW_MARGIN;
    int32_t room_h = static_cast<int32_t>(frame->height) - PREVIEW_MARGIN - 2 * PREVIEW_CHIP_GAP - CHIP_HEIGHT;
    double scale = std::min({ 1.0, static_cast<double>(room_w) / source.pixels->width,
                              static_cast<double>(room_h) / source.pixels->height });
    int32_t w = std::max(1, static_cast<int32_t>(source.pixels->width * scale + 0.5));
    int32_t h = std::max(1, static_cast<int32_t>(source.pixels->height * scale + 0.5));

    stlxgfx_surface_t* fitted = scale < 1.0 ? shrink_capture(source.pixels, source.keeps_alpha, w, h) : nullptr;
    const stlxgfx_surface_t* image = fitted ? fitted : source.pixels;
    if (scale < 1.0 && !fitted) {
        return;
    }

    damage_list::rect at = { (static_cast<int32_t>(frame->width) - w) / 2, PREVIEW_MARGIN + (room_h - h) / 2, w, h };
    if (source.keeps_alpha) {
        stlxgfx_blit_alpha(frame, at.x, at.y, image, 0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    } else {
        draw_shadow(frame, at);
        stlxgfx_blit(frame, at.x, at.y, image, 0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        blend_between(frame, grown(at, 1), 0, at, 0, STLXTHEME_LINE);
    }

    stlxgfx_destroy_surface(fitted);
    draw_preview_chip(frame, frost, source, at.y + h + PREVIEW_CHIP_GAP);
}

void capture_preview::open(std::vector<frozen_frame> frames, const preview_source& source,
                           const stlxgfx_surface_t* frost) {
    m_frames = std::move(frames);
    m_is_open = true;
    for (frozen_frame& frame : m_frames) {
        if (frame.surface) {
            stlxgfx_fill_rect_blend(frame.surface, 0, 0, frame.surface->width, frame.surface->height, DIM_COLOR);
        }
    }

    bool has_frame = source.output < m_frames.size() && m_frames[source.output].surface;
    if (has_frame && source.pixels) {
        draw_preview(m_frames[source.output].surface, frost, source);
    }
}

void capture_preview::close() {
    m_is_open = false;
    m_frames.clear();
}

void capture_preview::draw(uint32_t output, stlxgfx_surface_t* back, const damage_list::rect& clip) const {
    stlxgfx_surface_t* view = view_of(back, clip);
    if (!view) {
        return;
    }

    copy_opaque(view, output < m_frames.size() ? m_frames[output].surface : nullptr, clip);
    stlxgfx_destroy_surface(view);
}
