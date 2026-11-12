#ifndef STLXDM_CAPTURE_HPP
#define STLXDM_CAPTURE_HPP

#include "damage.hpp"
#include "picture.hpp"

#include <stlxgfx/surface.h>

#include <memory>

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

/* A copy of `r` of `frame`, in the frame's pixels, which the caller destroys, or null without memory */
stlxgfx_surface_t* copy_frozen_rect(const frozen_frame& frame, const damage_list::rect& r);

#endif
