#include "capture.hpp"

#include <cstring>

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
