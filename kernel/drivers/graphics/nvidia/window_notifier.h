// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_WINDOW_NOTIFIER_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_WINDOW_NOTIFIER_H

#include "common/types.h"
#include "dma/dma.h"

namespace drivers::nvidia {

// NVIDIA's NV_DMA_EVO_NOTIFIER_SIZE, the bytes of a window notifier's page, and the bytes of each slot presents use
constexpr uint64_t WINDOW_NOTIFIER_BYTES      = 0x1000;
constexpr uint32_t WINDOW_NOTIFIER_SLOT_BYTES = 16;

/**
 * A window channel's notifier, a page of system memory whose slots, each a
 * 16-byte completion notifier, the display engine writes as it performs the
 * window's updates that name them: begun once a head takes the update's state
 * at a vertical blank, finished once the update completes. Presents take the
 * first `NOTIFIER_SLOTS` slots in turn, each cleared before its update is
 * written. `display_engine` holds the lit window's, allocating its page and
 * binding its context DMA to the window's channel in `set_up`, and never frees
 * the page, as `completion_notifier` describes for the core channel's. A page
 * whose binding finds no room is kept too, unused, rather than freed by a call
 * made only for that path.
 */
class window_notifier {
public:
    // Allocates the page as `completion_notifier::allocate` does, at most once. `OK`, or `ERR_NO_MEMORY` logged
    int32_t allocate(const dma::device_profile& card);

    // The page's physical address, 0 until `allocate` succeeds
    uint64_t physical_address() const;

    // Writes the not-begun status into `slot`, below `NOTIFIER_SLOTS`, once `allocate` has succeeded
    void clear(uint32_t slot);

    // Whether `slot` reads begun or finished, so a head took the state its update wrote. Never waits
    bool written(uint32_t slot) const;

private:
    friend struct window_notifier_test_access;

    dma::buffer m_page = {};
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_WINDOW_NOTIFIER_H
