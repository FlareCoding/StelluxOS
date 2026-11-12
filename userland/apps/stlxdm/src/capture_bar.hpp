#ifndef STLXDM_CAPTURE_BAR_HPP
#define STLXDM_CAPTURE_BAR_HPP

#include "damage.hpp"

#include <stlxgfx/surface.h>

#include <cstdint>

/* What the capture overlay aims at: a region, a window, or a whole output */
enum class capture_mode { region, window, screen };

/* The capture bar: the modes as a segmented row, Capture and a close button, on glass at the bottom middle of the main
 * output. It paints into a picture once per change, which the overlay blends over its frozen pixels. */
class capture_bar {
public:
    enum class control { none, region, window, screen, capture, close };

    capture_bar();
    ~capture_bar();

    capture_bar(const capture_bar&) = delete;
    capture_bar& operator=(const capture_bar&) = delete;

    /* Shows the bar at the bottom middle of the output at `place` on the desktop, `mode` chosen, on the glass of
     * `frost`, the output's. False when its picture could not be made. */
    bool show(const damage_list::rect& place, const stlxgfx_surface_t* frost, capture_mode mode);
    void hide();

    bool shown() const { return m_picture != nullptr; }
    const damage_list::rect& rect() const { return m_rect; }

    control control_at(int32_t x, int32_t y) const;

    /* Each repaints the bar and damages it when it changes what the bar shows */
    void choose(capture_mode mode, damage_list& damage);
    void light(control lit, damage_list& damage);

    /* Blends the bar into `view`, whose first pixel shows the desktop point (`ox`, `oy`) */
    void draw(stlxgfx_surface_t* view, int32_t ox, int32_t oy) const;

private:
    static constexpr int32_t CONTROLS = 6;

    void lay_out(const damage_list::rect& place);
    void paint();
    void paint_mode(capture_mode mode);

    damage_list::rect        m_rect = {};
    damage_list::rect        m_place = {};
    damage_list::rect        m_controls[CONTROLS] = {};  /* inside the bar, by control */
    const stlxgfx_surface_t* m_frost = nullptr;
    capture_mode             m_mode = capture_mode::region;
    control                  m_lit = control::none;
    stlxgfx_surface_t*       m_picture = nullptr;

    /* Each mode's glyph at rest and lit, by mode, and the close button's */
    stlxgfx_surface_t* m_glyphs[3] = {};
    stlxgfx_surface_t* m_lit_glyphs[3] = {};
    stlxgfx_surface_t* m_close_glyph = nullptr;
    stlxgfx_surface_t* m_close_glyph_lit = nullptr;
};

/* The control that chooses `mode` */
capture_bar::control control_of(capture_mode mode);

#endif
