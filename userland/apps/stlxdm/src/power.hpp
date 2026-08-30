#ifndef STLXDM_POWER_HPP
#define STLXDM_POWER_HPP

#include "damage.hpp"

#include <stlxgfx/surface.h>

#include <cstdint>

/* The power overlay: a dimmed desktop with two orbs, restart and
 * shut down, above every window and below only the cursor. Orbs
 * reveal with a short stagger, commit by holding until the progress
 * ring closes, and the chosen light collapses the screen to black
 * before the platform call. The dock's right edge carries the star
 * that opens it. */
class dm_power {
public:
    enum class state {
        closed,
        opening,
        open,
        closing,
        committing,
    };

    /* Opens the label faces and reserves the collapse backdrop.
     * Returns 0, or -1 when a face is missing. */
    int init(uint32_t screen_w, uint32_t screen_h,
             uint32_t taskbar_height);
    void shutdown();

    /* True while the overlay owns every input source */
    bool active() const { return m_state != state::closed; }

    /* True during the reveal, the close, and the commit collapse,
     * the phases that repaint the whole screen per frame */
    bool transitioning() const {
        return m_state == state::opening || m_state == state::closing ||
               m_state == state::committing;
    }

    /* True while frames must keep coming: any transition, or a hold
     * winding its progress ring */
    bool animating() const {
        return transitioning() ||
               (m_state == state::open && m_press >= 0);
    }

    /* True while the collapse repaints the whole screen from the
     * cached backdrop instead of composing the scene */
    bool collapsing() const;

    void open();
    void dismiss();

    /* Advances phase timers and completes an elapsed hold */
    void update();

    /* Pointer input while the overlay is active, and star hover
     * tracking while it is closed */
    void on_motion(int32_t x, int32_t y);
    void on_press(int32_t x, int32_t y);
    void on_release();

    bool star_hit(int32_t x, int32_t y) const;
    bool star_hover() const { return m_star_hover; }
    int32_t hover_choice() const { return m_hover; }

    /* Bounding box of one orb including glow, hold ring, and label */
    damage_list::rect orb_box(int32_t choice) const;

    /* The dim, the hint line, and the orbs, clipped to one compose
     * rect. Captures the collapse backdrop on full screen frames. */
    void draw_overlay(stlxgfx_surface_t* back,
                      const damage_list::rect& clip);

    /* The dock's power star, drawn only while the overlay is closed */
    void draw_star(stlxgfx_surface_t* back,
                   const damage_list::rect& clip);

    /* One collapse frame: the backdrop fading to black under the
     * contracting light. Covers the whole target. */
    void draw_collapse(stlxgfx_surface_t* back);

    /* Issues the chosen power operation once the collapse has fully
     * darkened. Only returns when the platform refused. */
    void run_action();

private:
    void orb_center(int32_t choice, float* cx, float* cy) const;
    int32_t choice_at(int32_t x, int32_t y) const;
    float phase(uint32_t duration_ms) const;
    void draw_orbs(stlxgfx_surface_t* s, int32_t ox, int32_t oy);
    void draw_collapse_light(stlxgfx_surface_t* s, int32_t ox,
                             int32_t oy);

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    state m_state = state::closed;
    uint64_t m_phase_start_ns = 0;

    int32_t m_star_cx = 0;
    int32_t m_star_cy = 0;
    bool m_star_hover = false;

    int32_t m_hover = -1;      /* orb under the pointer, -1 off both */
    int32_t m_press = -1;      /* orb being held */
    uint64_t m_hold_start_ns = 0;

    int32_t m_commit = -1;
    bool m_action_ready = false;

    /* The dimmed desktop is identical every collapse frame, so it is
     * captured once and only faded from there. */
    stlxgfx_surface_t* m_backdrop = nullptr;
    bool m_backdrop_valid = false;
};

#endif
