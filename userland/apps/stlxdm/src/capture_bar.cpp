#include "capture_bar.hpp"

#include "capture.hpp"
#include "capture_glyphs.hpp"
#include "shell/draw.hpp"

#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstring>

/* The bar is 40 px of controls 32 px tall inside 4 px of padding, standing 12 px clear of the dock, whose shelf rises
 * 16 px above its output's foot */
constexpr int32_t BAR_HEIGHT = 40;
constexpr int32_t BAR_PADDING = 4;
constexpr int32_t BAR_FOOT = 16 + STLXTHEME_DOCK_HEIGHT + 12;
constexpr int32_t CONTROL_HEIGHT = 32;
constexpr int32_t CONTROL_GAP = 4;
constexpr int32_t SEGMENT_GAP = 2;
constexpr int32_t SEGMENT_PADDING_X = 12;
constexpr int32_t GLYPH_SIZE = 16;
constexpr int32_t GLYPH_GAP = 8;
constexpr int32_t SEPARATOR_MARGIN = 10;
constexpr int32_t SEPARATOR_HEIGHT = 20;
constexpr int32_t BUTTON_PADDING_X = 14;

/* The options' panel sits 8 px above the bar, its rows 32 px tall, its right edge under the Options button's */
constexpr int32_t PANEL_WIDTH = 264;
constexpr int32_t PANEL_GAP = 8;
constexpr int32_t ROW_HEIGHT = 32;
constexpr int32_t ROW_PADDING_X = 12;

constexpr int32_t MODES = 3;
static const char* const MODE_LABELS[MODES] = { "Region", "Window", "Screen" };
static const capture_glyph MODE_GLYPHS[MODES] = { capture_glyph::region, capture_glyph::window, capture_glyph::screen };
constexpr const char* OPTIONS_LABEL = "Options";
constexpr const char* CAPTURE_LABEL = "Capture";

constexpr int32_t ROWS = 4;
static const capture_bar::control ROW_CONTROLS[ROWS] = { capture_bar::control::timer, capture_bar::control::shadow,
                                                         capture_bar::control::pointer,
                                                         capture_bar::control::remember };
static const char* const ROW_LABELS[ROWS] = { "Timer", "Window shadow", "Show pointer", "Remember last region" };

static bool contains(const damage_list::rect& r, int32_t x, int32_t y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static stlxgfx_font* label_font() {
    return shell_font(STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE);
}

static int32_t label_width(const char* label) {
    stlxgfx_font* font = label_font();
    return font ? stlxgfx_text_width(font, label, strlen(label)) : 0;
}

/* `label` from `x` on a line centered in `r`'s rows */
static void draw_label(stlxgfx_surface_t* target, const damage_list::rect& r, int32_t x, const char* label,
                       uint32_t color) {
    stlxgfx_font* font = label_font();
    if (!font) {
        return;
    }

    stlxgfx_font_metrics metrics;
    stlxgfx_font_metrics_get(font, &metrics);
    int32_t baseline = r.y + (r.h + metrics.ascent - metrics.descent) / 2;
    stlxgfx_draw_text(target, font, x, baseline, label, strlen(label), color);
}

static void draw_glyph(stlxgfx_surface_t* target, const stlxgfx_surface_t* glyph, int32_t x, int32_t y) {
    if (glyph) {
        stlxgfx_blit_alpha(target, x, y, glyph, 0, 0, glyph->width, glyph->height);
    }
}

/* A button of the bar: a neutral face, lit under the pointer, on while `is_on` */
static void draw_button(stlxgfx_ctx_t* ctx, stlxgfx_surface_t* target, const damage_list::rect& r, const char* label,
                        bool is_lit, bool is_on) {
    uint32_t face = is_on ? STLXTHEME_SURFACE_ON : is_lit ? STLXTHEME_SURFACE_HOVER : STLXTHEME_SURFACE;
    uint32_t line = is_on ? STLXTHEME_LINE_ON : is_lit ? STLXTHEME_LINE_STRONG : STLXTHEME_LINE;
    uint32_t w = static_cast<uint32_t>(r.w);
    stlxgfx_ctx_fill_rounded_rect(ctx, r.x, r.y, w, CONTROL_HEIGHT, STLXTHEME_CONTROL_RADIUS, face);
    stlxgfx_ctx_draw_rounded_rect(ctx, r.x, r.y, w, CONTROL_HEIGHT, STLXTHEME_CONTROL_RADIUS, line);
    draw_label(target, r, r.x + BUTTON_PADDING_X, label, STLXTHEME_TEXT);
}

static const char* row_value(capture_bar::control row, const capture_options& options) {
    switch (row) {
    case capture_bar::control::timer:
        return options.timer == 0 ? "None" : options.timer == 5 ? "5 seconds" : "10 seconds";
    case capture_bar::control::shadow:
        return options.shadow ? "On" : "Off";
    case capture_bar::control::pointer:
        return options.pointer ? "On" : "Off";
    case capture_bar::control::remember:
        return options.remember_region ? "On" : "Off";
    default:
        return "";
    }
}

static int32_t index_of(capture_bar::control control) {
    return static_cast<int32_t>(control);
}

capture_bar::control control_of(capture_mode mode) {
    switch (mode) {
    case capture_mode::region:
        return capture_bar::control::region;
    case capture_mode::window:
        return capture_bar::control::window;
    case capture_mode::screen:
        return capture_bar::control::screen;
    }

    return capture_bar::control::none;
}

capture_bar::capture_bar() {
    for (int32_t i = 0; i < MODES; i++) {
        m_glyphs[i] = make_capture_glyph(MODE_GLYPHS[i], GLYPH_SIZE, STLXTHEME_TEXT_SECONDARY);
        m_lit_glyphs[i] = make_capture_glyph(MODE_GLYPHS[i], GLYPH_SIZE, STLXTHEME_TEXT);
    }

    m_close_glyph = make_capture_glyph(capture_glyph::close, GLYPH_SIZE, STLXTHEME_TEXT_SECONDARY);
    m_close_glyph_lit = make_capture_glyph(capture_glyph::close, GLYPH_SIZE, STLXTHEME_TEXT);
}

capture_bar::~capture_bar() {
    hide();
    for (int32_t i = 0; i < MODES; i++) {
        stlxgfx_destroy_surface(m_glyphs[i]);
        stlxgfx_destroy_surface(m_lit_glyphs[i]);
    }

    stlxgfx_destroy_surface(m_close_glyph);
    stlxgfx_destroy_surface(m_close_glyph_lit);
}

bool capture_bar::show(const damage_list::rect& place, const stlxgfx_surface_t* frost, capture_mode mode,
                       const capture_options& options) {
    hide();
    m_mode = mode;
    m_options = options;
    m_lit = control::none;

    m_place = place;
    m_frost = frost;
    lay_out(place);

    m_picture = stlxgfx_create_surface(static_cast<uint32_t>(m_rect.w), BAR_HEIGHT, 32, 16, 8, 0);
    if (!m_picture) {
        return false;
    }

    m_picture->flags = STLXGFX_SURFACE_STRAIGHT_ALPHA;
    paint();
    return true;
}

void capture_bar::hide() {
    stlxgfx_destroy_surface(m_picture);
    stlxgfx_destroy_surface(m_panel);
    m_picture = nullptr;
    m_panel = nullptr;
    m_rect = {};
    m_panel_rect = {};
}

bool capture_bar::contains(int32_t x, int32_t y) const {
    return m_picture && (::contains(m_rect, x, y) || (m_panel && ::contains(m_panel_rect, x, y)));
}

capture_bar::control capture_bar::control_at(int32_t x, int32_t y) const {
    if (m_panel && ::contains(m_panel_rect, x, y)) {
        for (control row : ROW_CONTROLS) {
            if (::contains(m_controls[index_of(row)], x - m_panel_rect.x, y - m_panel_rect.y)) {
                return row;
            }
        }

        return control::none;
    }

    if (!m_picture || !::contains(m_rect, x, y)) {
        return control::none;
    }

    for (int32_t i = index_of(control::region); i <= index_of(control::close); i++) {
        if (::contains(m_controls[i], x - m_rect.x, y - m_rect.y)) {
            return static_cast<control>(i);
        }
    }

    return control::none;
}

damage_list::rect capture_bar::bounds() const {
    return m_panel ? damage_list::union_of(m_rect, m_panel_rect) : m_rect;
}

void capture_bar::choose(capture_mode mode, damage_list& damage) {
    if (m_picture && mode != m_mode) {
        m_mode = mode;
        paint();
        damage.add(m_rect.x, m_rect.y, m_rect.w, m_rect.h);
    }
}

void capture_bar::light(control lit, damage_list& damage) {
    if (m_picture && lit != m_lit) {
        m_lit = lit;
        paint();
        paint_panel();
        damage_all(damage);
    }
}

void capture_bar::toggle_panel(damage_list& damage) {
    if (!m_picture) {
        return;
    }

    damage_all(damage);
    if (m_panel) {
        stlxgfx_destroy_surface(m_panel);
        m_panel = nullptr;
        m_panel_rect = {};
    } else {
        lay_out_panel();
        m_panel = stlxgfx_create_surface(static_cast<uint32_t>(m_panel_rect.w),
                                         static_cast<uint32_t>(m_panel_rect.h), 32, 16, 8, 0);
        if (m_panel) {
            m_panel->flags = STLXGFX_SURFACE_STRAIGHT_ALPHA;
        }
    }

    paint();
    paint_panel();
    damage_all(damage);
}

void capture_bar::change(control row, damage_list& damage) {
    switch (row) {
    case control::timer:
        m_options.timer = m_options.timer == 0 ? 5 : m_options.timer == 5 ? 10 : 0;
        break;
    case control::shadow:
        m_options.shadow = !m_options.shadow;
        break;
    case control::pointer:
        m_options.pointer = !m_options.pointer;
        break;
    case control::remember:
        m_options.remember_region = !m_options.remember_region;
        break;
    default:
        return;
    }

    paint_panel();
    damage_all(damage);
}

void capture_bar::draw(stlxgfx_surface_t* view, int32_t ox, int32_t oy) const {
    if (m_picture) {
        stlxgfx_blit_alpha(view, m_rect.x - ox, m_rect.y - oy, m_picture, 0, 0, m_picture->width, m_picture->height);
    }

    if (m_panel) {
        stlxgfx_blit_alpha(view, m_panel_rect.x - ox, m_panel_rect.y - oy, m_panel, 0, 0, m_panel->width,
                           m_panel->height);
    }
}

/* The modes from the left, a separator, then Options, Capture and close, the bar as wide as they need */
void capture_bar::lay_out(const damage_list::rect& place) {
    int32_t x = BAR_PADDING;
    for (int32_t i = 0; i < MODES; i++) {
        int32_t w = 2 * SEGMENT_PADDING_X + GLYPH_SIZE + GLYPH_GAP + label_width(MODE_LABELS[i]);
        m_controls[index_of(control::region) + i] = { x, BAR_PADDING, w, CONTROL_HEIGHT };
        x += w + (i + 1 < MODES ? SEGMENT_GAP : 0);
    }

    x += 2 * SEPARATOR_MARGIN + 1;
    for (control button : { control::options, control::capture }) {
        const char* label = button == control::options ? OPTIONS_LABEL : CAPTURE_LABEL;
        int32_t w = 2 * BUTTON_PADDING_X + label_width(label);
        m_controls[index_of(button)] = { x, BAR_PADDING, w, CONTROL_HEIGHT };
        x += w + CONTROL_GAP;
    }

    m_controls[index_of(control::close)] = { x, BAR_PADDING, CONTROL_HEIGHT, CONTROL_HEIGHT };
    x += CONTROL_HEIGHT + BAR_PADDING;
    m_rect = { place.x + (place.w - x) / 2, place.y + place.h - BAR_FOOT - BAR_HEIGHT, x, BAR_HEIGHT };
}

void capture_bar::lay_out_panel() {
    const damage_list::rect& options = m_controls[index_of(control::options)];
    int32_t h = ROWS * ROW_HEIGHT + 2 * BAR_PADDING;
    int32_t x = std::max(m_place.x, m_rect.x + options.x + options.w - PANEL_WIDTH);
    m_panel_rect = { x, m_rect.y - PANEL_GAP - h, PANEL_WIDTH, h };
    for (int32_t i = 0; i < ROWS; i++) {
        m_controls[index_of(ROW_CONTROLS[i])] = { BAR_PADDING, BAR_PADDING + i * ROW_HEIGHT,
                                                  PANEL_WIDTH - 2 * BAR_PADDING, ROW_HEIGHT };
    }
}

/* The chosen mode on the face and ring of a chosen segment, the control under the pointer lit, Options on while its
 * panel is open, all on the glass the overlay's chips use */
void capture_bar::paint() {
    stlxgfx_clear(m_picture, 0);
    int32_t x = m_rect.x - m_place.x;
    int32_t y = m_rect.y - m_place.y;
    draw_chip_glass(m_picture, x, y, m_frost, { x, y, m_rect.w, m_rect.h }, STLXTHEME_WINDOW_RADIUS);

    for (int32_t i = 0; i < MODES; i++) {
        paint_mode(static_cast<capture_mode>(i));
    }

    const damage_list::rect& last = m_controls[index_of(control::screen)];
    stlxgfx_fill_rect_blend(m_picture, last.x + last.w + SEPARATOR_MARGIN, (BAR_HEIGHT - SEPARATOR_HEIGHT) / 2, 1,
                            SEPARATOR_HEIGHT, STLXTHEME_LINE);

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, m_picture);
    draw_button(&ctx, m_picture, m_controls[index_of(control::options)], OPTIONS_LABEL, m_lit == control::options,
                m_panel != nullptr);
    draw_button(&ctx, m_picture, m_controls[index_of(control::capture)], CAPTURE_LABEL, m_lit == control::capture,
                false);

    const damage_list::rect& close = m_controls[index_of(control::close)];
    bool is_close_lit = m_lit == control::close;
    if (is_close_lit) {
        stlxgfx_ctx_fill_rounded_rect(&ctx, close.x, close.y, CONTROL_HEIGHT, CONTROL_HEIGHT, STLXTHEME_CONTROL_RADIUS,
                                      STLXTHEME_SURFACE_HOVER);
    }

    int32_t inset = (CONTROL_HEIGHT - GLYPH_SIZE) / 2;
    draw_glyph(m_picture, is_close_lit ? m_close_glyph_lit : m_close_glyph, close.x + inset, close.y + inset);
}

void capture_bar::paint_mode(capture_mode mode) {
    int32_t i = static_cast<int32_t>(mode);
    const damage_list::rect& r = m_controls[index_of(control_of(mode))];
    bool is_chosen = mode == m_mode;
    bool is_lit = control_of(mode) == m_lit;

    uint32_t w = static_cast<uint32_t>(r.w);
    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, m_picture);
    if (is_chosen) {
        stlxgfx_ctx_fill_rounded_rect(&ctx, r.x, r.y, w, CONTROL_HEIGHT, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_ON);
        stlxgfx_ctx_draw_rounded_rect(&ctx, r.x, r.y, w, CONTROL_HEIGHT, STLXTHEME_CONTROL_RADIUS, STLXTHEME_LINE_ON);
    } else if (is_lit) {
        stlxgfx_ctx_fill_rounded_rect(&ctx, r.x, r.y, w, CONTROL_HEIGHT, STLXTHEME_CONTROL_RADIUS,
                                      STLXTHEME_SURFACE_HOVER);
    }

    bool is_bright = is_chosen || is_lit;
    int32_t glyph_x = r.x + SEGMENT_PADDING_X;
    draw_glyph(m_picture, is_bright ? m_lit_glyphs[i] : m_glyphs[i], glyph_x, r.y + (r.h - GLYPH_SIZE) / 2);
    draw_label(m_picture, r, glyph_x + GLYPH_SIZE + GLYPH_GAP, MODE_LABELS[i],
               is_bright ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY);
}

/* Each option's name on the left and its value on the right, the row under the pointer lit */
void capture_bar::paint_panel() {
    if (!m_panel) {
        return;
    }

    stlxgfx_clear(m_panel, 0);
    int32_t x = m_panel_rect.x - m_place.x;
    int32_t y = m_panel_rect.y - m_place.y;
    draw_chip_glass(m_panel, x, y, m_frost, { x, y, m_panel_rect.w, m_panel_rect.h }, STLXTHEME_WINDOW_RADIUS);

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, m_panel);
    for (int32_t i = 0; i < ROWS; i++) {
        const damage_list::rect& r = m_controls[index_of(ROW_CONTROLS[i])];
        if (m_lit == ROW_CONTROLS[i]) {
            stlxgfx_ctx_fill_rounded_rect(&ctx, r.x, r.y, static_cast<uint32_t>(r.w), ROW_HEIGHT,
                                          STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_HOVER);
        }

        const char* value = row_value(ROW_CONTROLS[i], m_options);
        draw_label(m_panel, r, r.x + ROW_PADDING_X, ROW_LABELS[i], STLXTHEME_TEXT);
        draw_label(m_panel, r, r.x + r.w - ROW_PADDING_X - label_width(value), value, STLXTHEME_TEXT_SECONDARY);
    }
}

void capture_bar::damage_all(damage_list& damage) const {
    damage_list::rect all = bounds();
    damage.add(all.x, all.y, all.w, all.h);
}
