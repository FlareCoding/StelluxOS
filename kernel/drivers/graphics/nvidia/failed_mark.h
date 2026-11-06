#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FAILED_MARK_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FAILED_MARK_H

#include "sync/atomic.h"

namespace drivers::nvidia {

/**
 * The mark that the GPU failed, set for good once the driver can no longer
 * trust the card, as after a failed update of a GPU context's page tables or a
 * reply the GSP's firmware missed. Every part that sets it or must refuse work
 * once it is set must hold a reference to the GPU's one mark. Both calls are
 * safe in an interrupt handler and under a spinlock.
 */
class failed_mark {
public:
    void set() { m_is_set.store_release(true); }

    bool is_set() const { return m_is_set.load_acquire(); }

private:
    sync::atomic<bool> m_is_set{false};
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FAILED_MARK_H
