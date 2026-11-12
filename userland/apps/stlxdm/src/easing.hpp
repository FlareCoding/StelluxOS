#ifndef STLXDM_EASING_HPP
#define STLXDM_EASING_HPP

#include <cstdint>

/* Eased progress runs from 0 to this */
constexpr int64_t PROGRESS_ONE = 1 << 16;
constexpr uint32_t PROGRESS_BITS = 16;

/* Cubic ease out of the elapsed share of `duration`, in `PROGRESS_ONE` units, the curve every desktop motion follows */
inline int64_t ease_out(uint64_t elapsed, uint64_t duration) {
    if (elapsed >= duration) {
        return PROGRESS_ONE;
    }

    int64_t remaining = PROGRESS_ONE - static_cast<int64_t>(elapsed * PROGRESS_ONE / duration);
    return PROGRESS_ONE - ((remaining * remaining >> PROGRESS_BITS) * remaining >> PROGRESS_BITS);
}

inline int32_t interpolate(int32_t from, int32_t to, int64_t progress) {
    return from + static_cast<int32_t>(static_cast<int64_t>(to - from) * progress >> PROGRESS_BITS);
}

#endif
