#ifndef STLXDM_PLACEMENT_HPP
#define STLXDM_PLACEMENT_HPP

#include "damage.hpp"

/* How a toplevel shows: where it was last moved and sized, or with its
 * frame filling its output's work area within its size hints */
enum class window_state {
    normal,
    maximized,
};

/* Where a toplevel shows. `floating` is the content rect in desktop coordinates that toggling back to normal
 * restores and whose size a title drag restores under the pointer, recorded as the window leaves normal and moved
 * with its output. */
struct window_placement {
    window_state      state = window_state::normal;
    damage_list::rect floating;
};

#endif
