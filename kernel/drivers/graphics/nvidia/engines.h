#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/copy_channel.h"
#include "drivers/graphics/nvidia/engine_table.h"
#include "drivers/graphics/nvidia/failed_mark.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "sync/atomic.h"

namespace drivers::nvidia {

/**
 * The GPU's engines as the driver task brings them up once `attach()` has
 * returned `OK`: the engine table, the kernel's copy channel on the first
 * asynchronous copy engine, and that engine's nonstall vector. The driver
 * object holds one for its life.
 *
 * `bring_up` waits for the firmware and for video memory, so the driver task
 * must call it, and only once. The card's interrupt handler may call
 * `service_interrupt` at any time, and the call does nothing before `bring_up`
 * creates the copy channel or after it masks the vector.
 *
 * After a timeout the hung channel and the memory it fills stay for good.
 * After another error, only what a refused release or a failed unmap leaves
 * the firmware or the GPU able to reach stays.
 */
class engines {
public:
    // Engines holding nothing, using `fw`, `objects`, `memory` and `failed`, which must outlive them
    engines(firmware& fw, const gpu_objects& objects, video_memory& memory, failed_mark& failed);

    engines(const engines&) = delete;
    engines& operator=(const engines&) = delete;

    /**
     * @brief Read the engine table, create the kernel's copy channel on the
     * first asynchronous copy engine with `bar0` for its doorbell, enable the
     * engine's nonstall vector in `chip`'s leaves, and check the channel by
     * having it zero and then fill video memory, each read back through BAR1.
     * @return `OK`, `ERR_BAD_STATE` when no asynchronous copy engine exists or
     * a word reads back wrong, `ERR_TIMEOUT` when a copy hangs the channel, or
     * the failing step's error. Every error is logged and leaves the vector
     * masked.
     */
    int32_t bring_up(registers& bar0, const dma::device_profile& card, const chip_info& chip);

    /**
     * @brief Clear the copy engine's nonstall vector when it is pending and
     * have the copy channel read its semaphore, from the card's interrupt
     * handler. Does nothing before `bring_up` creates the copy channel or after
     * it masks the vector. Takes the copy channel's spinlock and never sleeps.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void service_interrupt(registers& bar0);

private:
    friend struct engines_test_access;

    // Fills the check's memory, reads it back and logs each copy's time, keeping the memory for good after a timeout
    int32_t check_copy_channel();

    // Has the copy channel fill the checked bytes with `value`, then reads them and the page after them back
    int32_t fill_and_compare(const vram_range& range, uintptr_t virt, uint32_t value, uint64_t& took_ns);

    firmware&          m_firmware;
    const gpu_objects& m_objects;
    video_memory&      m_memory;
    failed_mark&       m_failed;
    engine_table       m_table;

    // The kernel's copy channel in `m_copy_channel_bytes`, never destructed, since the handler may still reach it
    copy_channel*                 m_copy_channel = nullptr;
    alignas(copy_channel) uint8_t m_copy_channel_bytes[sizeof(copy_channel)];

    uint32_t           m_copy_vector = 0;
    sync::atomic<bool> m_is_copy_vector_on{false}; // stored with release once the channel exists, loaded with acquire
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H
