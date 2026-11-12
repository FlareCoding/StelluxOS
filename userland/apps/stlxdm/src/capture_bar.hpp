#ifndef STLXDM_CAPTURE_BAR_HPP
#define STLXDM_CAPTURE_BAR_HPP

#include "damage.hpp"

#include <stlxgfx/surface.h>

#include <cstdint>

/* What the capture overlay aims at: a region, a window, or a whole output */
enum class capture_mode { region, window, screen };

/* The capture bar's options, which last for the session from the [screenshot] keys: the seconds its timer counts
 * down, 0, 5 or 10, and whether a window keeps its shadow, a capture shows the pointer, and the bar keeps the last
 * region */
struct capture_options {
    uint32_t timer = 0;
    bool     shadow = true;
    bool     pointer = false;
    bool     remember_region = true;
};

/* The capture bar: the modes as a segmented row, Options, Capture and a close button, on glass at the bottom middle of
 * the main output, with the options' panel above it while open. It paints into pictures once per change, which the
 * overlay blends over its frozen pixels. */
class capture_bar {
public:
    enum class control { none, region, window, screen, options, capture, close, timer, shadow, pointer, remember };

    capture_bar();
    ~capture_bar();

    capture_bar(const capture_bar&) = delete;
    capture_bar& operator=(const capture_bar&) = delete;

    /* Shows the bar at the bottom middle of the output at `place` on the desktop, `mode` chosen and `options` set, on
     * the glass of `frost`, the output's. False when its picture could not be made. */
    bool show(const damage_list::rect& place, const stlxgfx_surface_t* frost, capture_mode mode,
              const capture_options& options);
    void hide();

    bool shown() const { return m_picture != nullptr; }
    const capture_options& options() const { return m_options; }

    /* Whether (`x`, `y`) lies on the bar or on its open panel, and the control there */
    bool contains(int32_t x, int32_t y) const;
    control control_at(int32_t x, int32_t y) const;

    /* The rects the bar and its open panel cover on the desktop, which draws blend into */
    damage_list::rect bounds() const;

    /* Each repaints what it changes and damages it */
    void choose(capture_mode mode, damage_list& damage);
    void light(control lit, damage_list& damage);
    void toggle_panel(damage_list& damage);

    /* A row of the panel changes its option: the timer to its next choice, a switch to its other side */
    void change(control row, damage_list& damage);

    /* Blends the bar and its open panel into `view`, whose first pixel shows the desktop point (`ox`, `oy`) */
    void draw(stlxgfx_surface_t* view, int32_t ox, int32_t oy) const;

private:
    static constexpr int32_t CONTROLS = 11;

    void lay_out(const damage_list::rect& place);
    void lay_out_panel();
    void paint();
    void paint_mode(capture_mode mode);
    void paint_panel();
    void damage_all(damage_list& damage) const;

    damage_list::rect        m_rect = {};
    damage_list::rect        m_place = {};
    damage_list::rect        m_controls[CONTROLS] = {};  /* inside the bar, or the panel for its rows, by control */
    const stlxgfx_surface_t* m_frost = nullptr;
    capture_mode             m_mode = capture_mode::region;
    capture_options          m_options;
    control                  m_lit = control::none;
    stlxgfx_surface_t*       m_picture = nullptr;

    /* The options' panel above the bar while open, and its place on the desktop */
    stlxgfx_surface_t* m_panel = nullptr;
    damage_list::rect  m_panel_rect = {};

    /* Each mode's glyph at rest and lit, by mode, and the close button's */
    stlxgfx_surface_t* m_glyphs[3] = {};
    stlxgfx_surface_t* m_lit_glyphs[3] = {};
    stlxgfx_surface_t* m_close_glyph = nullptr;
    stlxgfx_surface_t* m_close_glyph_lit = nullptr;
};

/* The control that chooses `mode` */
capture_bar::control control_of(capture_mode mode);

#endif
