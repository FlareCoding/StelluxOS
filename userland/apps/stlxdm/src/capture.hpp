#ifndef STLXDM_CAPTURE_HPP
#define STLXDM_CAPTURE_HPP

#include "damage.hpp"
#include "picture.hpp"

#include <stlxgfx/surface.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* One output's frame as it stood when a capture began, the pointer left out: in stlxdm's memory, or in a picture the
 * GPU composed it into, with a surface over its pixels either way */
struct frozen_frame {
    std::unique_ptr<picture> gpu_pixels;
    stlxgfx_surface_t*       surface = nullptr;
    damage_list::rect        place = {};  /* the output's rect on the desktop */

    frozen_frame() = default;
    ~frozen_frame() { stlxgfx_destroy_surface(surface); }

    frozen_frame(frozen_frame&& other) noexcept
        : gpu_pixels(std::move(other.gpu_pixels)), surface(other.surface), place(other.place) {
        other.surface = nullptr;
    }

    frozen_frame& operator=(frozen_frame&& other) noexcept {
        if (this != &other) {
            stlxgfx_destroy_surface(surface);
            gpu_pixels = std::move(other.gpu_pixels);
            surface = other.surface;
            place = other.place;
            other.surface = nullptr;
        }

        return *this;
    }

    frozen_frame(const frozen_frame&) = delete;
    frozen_frame& operator=(const frozen_frame&) = delete;
};

/* A window the capture overlay can light: the server's window, checked against its windows before use, its frame on
 * the desktop with the radius of its corners, and the title its tag names */
struct capture_window {
    const void*       id = nullptr;
    damage_list::rect frame = {};
    int32_t           radius = 0;
    std::string       title;
    damage_list::rect bounds = {};  /* the frame and its shadow, what a capture with the shadow takes */
};

/* What a capture takes: a rect of one output in that output's pixels, or a window alone, with its shadow or without */
struct capture_target {
    uint32_t          output = 0;
    damage_list::rect rect = {};
    const void*       window = nullptr;
    bool              has_shadow = true;
};

/* The frost an output's glass is drawn from, null for an output not served */
using frost_lookup = std::function<const stlxgfx_surface_t*(uint32_t output)>;

/* The room between a chip's pieces, as the palette's footer spaces a label and its keys */
constexpr int32_t PIECE_GAP = 6;

/* One piece of a chip's line: words in a face, size and color, or with no face a key's name drawn as a keycap, and the
 * room after it */
struct chip_piece {
    std::string text;
    const char* face = nullptr;
    float       size = 0;
    uint32_t    color = 0;
    int32_t     gap = PIECE_GAP;
};

/* The capture overlay: every output frozen and dimmed, and either a region dragged on one undimmed inside an amber
 * hairline or the window under the pointer lit. It takes keys and the pointer, each change damaging what it repaints. */
class capture_overlay {
public:
    bool active() const { return m_is_open; }

    void set_frost_lookup(frost_lookup lookup) { m_frost_of = std::move(lookup); }

    /* Opens over `frames`, one per output by index and empty for an output not served, aiming at a region, with
     * `windows` top first and the pointer at (`x`, `y`) */
    void open(std::vector<frozen_frame> frames, std::vector<capture_window> windows, int32_t x, int32_t y);
    void close();

    /* The pointer moved with the CHORD_MOD_* bits `mods` held: Shift keeps one side of the region, Alt grows it from
     * the center, and a held Space moves it */
    void on_motion(int32_t x, int32_t y, uint8_t mods, damage_list& damage);

    /* The primary button went down: a drag starts on the output under it, or Alt marks a window's click shadowless */
    void on_press(int32_t x, int32_t y, uint8_t mods, damage_list& damage);

    /* The primary button came up, true with what to take: a dragged region, or the window clicked. A click that
     * dragged nothing ends the drag and leaves the overlay open. */
    bool on_release(capture_target* target, damage_list& damage);

    /* Space moves the region while a drag holds it, and otherwise switches between a region and windows */
    void press_space(damage_list& damage);
    void release_space();

    uint32_t cursor_shape() const;

    /* Draws output `output`'s overlay inside `clip`, in its pixels, every pixel opaque */
    void draw(uint32_t output, stlxgfx_surface_t* back, const damage_list::rect& clip) const;

    const frozen_frame& frame(uint32_t output) const { return m_frames[output]; }

private:
    enum class mode { region, window };
    enum class axis { none, horizontal, vertical };

    void follow(int32_t x, int32_t y, uint8_t mods);
    void move_region(int32_t x, int32_t y);
    int32_t window_at(int32_t x, int32_t y) const;
    uint32_t output_at(int32_t x, int32_t y) const;
    uint32_t pointer_output() const;
    std::string tag_text() const;
    damage_list::rect chip_rect() const;
    std::string chip_words() const;
    void damage_changes(damage_list& damage);
    void damage_everything(damage_list& damage) const;
    void draw_region(stlxgfx_surface_t* view, int32_t ox, int32_t oy, uint32_t output) const;
    void draw_window(stlxgfx_surface_t* view, int32_t ox, int32_t oy) const;
    void draw_chip(stlxgfx_surface_t* view, int32_t ox, int32_t oy, uint32_t output) const;

    bool                        m_is_open = false;
    std::vector<frozen_frame>   m_frames;
    std::vector<capture_window> m_windows;
    frost_lookup                m_frost_of;
    mode                        m_mode = mode::region;
    int32_t                     m_pointer_x = 0;
    int32_t                     m_pointer_y = 0;
    uint8_t                     m_mods = 0;

    /* The window under the pointer, -1 for none, and the one a press began on, with Alt held at the press or not */
    int32_t m_lit = -1;
    int32_t m_pressed = -1;
    bool    m_press_has_shadow = true;

    /* The drag: the output it stays on, where it began, and the region in desktop coordinates */
    bool              m_is_dragging = false;
    uint32_t          m_output = 0;
    int32_t           m_anchor_x = 0;
    int32_t           m_anchor_y = 0;
    damage_list::rect m_region = {};

    /* Space held moves the region from the pointer's last place, and Shift keeps the region it found, letting only
     * the edge of the axis the pointer moved along first follow it */
    bool              m_is_space_held = false;
    int32_t           m_move_x = 0;
    int32_t           m_move_y = 0;
    axis              m_kept_axis = axis::none;
    bool              m_is_shift_held = false;
    damage_list::rect m_kept_region = {};
    int32_t           m_shift_x = 0;
    int32_t           m_shift_y = 0;

    /* What the last draws showed, which a change repaints */
    damage_list::rect m_shown_outline = {};
    damage_list::rect m_shown_chip = {};
    std::string m_shown_words;
};

/* A copy of `r` of `frame`, in the frame's pixels, which the caller destroys, or null without memory */
stlxgfx_surface_t* copy_frozen_rect(const frozen_frame& frame, const damage_list::rect& r);

/* `pixels` shrunk to `w` by `h`, each pixel the average of those it covers so text stays legible small, in straight
 * alpha when `keeps_alpha` and opaque otherwise. Null without memory. */
stlxgfx_surface_t* shrink_capture(const stlxgfx_surface_t* pixels, bool keeps_alpha, int32_t w, int32_t h);

/* Draws glass in a hairline at `box` into `target`, whose first pixel shows the output pixel (`x`, `y`), from the
 * output's `frost`, or on the menu's ink without one. Inside its rounded edge every pixel ends opaque, so it draws into
 * a straight alpha picture as into a frame. */
void draw_chip_glass(stlxgfx_surface_t* target, int32_t x, int32_t y, const stlxgfx_surface_t* frost,
                     const damage_list::rect& box, int32_t radius);

#endif
