#ifndef STLXDM_CAPTURE_HPP
#define STLXDM_CAPTURE_HPP

#include "damage.hpp"
#include "picture.hpp"

#include <stlxgfx/surface.h>

#include <cstdint>
#include <functional>
#include <memory>
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

/* What a capture takes: a rect of one output, in that output's pixels */
struct capture_target {
    uint32_t          output = 0;
    damage_list::rect rect = {};
};

/* The frost an output's glass is drawn from, null for an output not served */
using frost_lookup = std::function<const stlxgfx_surface_t*(uint32_t output)>;

/* The capture overlay: every output frozen and dimmed, and a region dragged on one undimmed inside an amber hairline,
 * with a tag by the crosshair saying where it points and then the region's size. It takes keys and the pointer while
 * open, each change damaging what it repaints. */
class capture_overlay {
public:
    bool active() const { return m_is_open; }

    void set_frost_lookup(frost_lookup lookup) { m_frost_of = std::move(lookup); }

    /* Opens over `frames`, one per output by index and empty for an output not served, with the pointer at
     * (`x`, `y`) */
    void open(std::vector<frozen_frame> frames, int32_t x, int32_t y);
    void close();

    /* The pointer moved with the CHORD_MOD_* bits `mods` held: Shift keeps one side of the region, Alt grows it from
     * the center, and a held Space moves it */
    void on_motion(int32_t x, int32_t y, uint8_t mods, damage_list& damage);

    /* The primary button went down, starting a drag on the output under it */
    void on_press(int32_t x, int32_t y, damage_list& damage);

    /* The primary button came up, true with the region to take when a drag ended. A click that dragged nothing ends
     * the drag and leaves the overlay open. */
    bool on_release(capture_target* target, damage_list& damage);

    void hold_space(bool is_held);

    /* Draws output `output`'s overlay inside `clip`, in its pixels, every pixel opaque */
    void draw(uint32_t output, stlxgfx_surface_t* back, const damage_list::rect& clip) const;

    const frozen_frame& frame(uint32_t output) const { return m_frames[output]; }

private:
    enum class axis { none, horizontal, vertical };

    void follow(int32_t x, int32_t y, uint8_t mods);
    void move_region(int32_t x, int32_t y);
    uint32_t output_at(int32_t x, int32_t y) const;
    uint32_t pointer_output() const;
    std::string tag_text() const;
    damage_list::rect chip_rect() const;
    std::string chip_words() const;
    void damage_changes(damage_list& damage);
    void draw_tag(stlxgfx_surface_t* view, int32_t ox, int32_t oy, uint32_t output) const;

    bool                      m_is_open = false;
    std::vector<frozen_frame> m_frames;
    frost_lookup              m_frost_of;
    int32_t                   m_pointer_x = 0;
    int32_t                   m_pointer_y = 0;

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

#endif
