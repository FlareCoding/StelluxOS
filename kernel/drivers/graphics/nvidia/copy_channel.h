#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_COPY_CHANNEL_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_COPY_CHANNEL_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/address_space.h"
#include "drivers/graphics/nvidia/channel.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/engine_table.h"
#include "drivers/graphics/nvidia/failed_mark.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace drivers::nvidia {

// How long the GPU may take with a submission before the driver counts its channel as hung
constexpr uint64_t GPU_SUBMIT_TIMEOUT_NS = 7000000000;

// The slots of the copy channel's push buffer (the memory holding its method words), each holding one fill
constexpr uint32_t COPY_SLOTS = 16;

/**
 * The kernel's copy channel: a privileged channel on an asynchronous copy
 * engine, in an address space of its own, that fills video memory by physical
 * address. Each fill ends with a release sequence that writes the low 32 bits
 * of the fill's sequence number to the channel's semaphore.
 *
 * `create` and `destroy` wait for the firmware and for video memory, so they
 * must run on the driver task, and `fill` must not overlap them. `fill` sleeps,
 * so it must not run in an interrupt handler or under a spinlock.
 *
 * A fill still running `GPU_SUBMIT_TIMEOUT_NS` after it became the oldest one
 * running marks the channel hung until `destroy`: every later fill, and every
 * waiting fill whose copy has not finished, returns `ERR_TIMEOUT`. A caller
 * given `ERR_TIMEOUT` must keep the memory it asked filled, since the GPU may
 * still write it. Memory the GPU may still reach after a refused release or a
 * failed unmap stays allocated for good.
 */
class copy_channel {
public:
    // A channel holding nothing, using `fw`, `objects`, `memory`, `bar0` and `failed`, which must outlive it
    copy_channel(firmware& fw, const gpu_objects& objects, video_memory& memory, registers bar0, failed_mark& failed);

    copy_channel(const copy_channel&) = delete;
    copy_channel& operator=(const copy_channel&) = delete;

    /**
     * @brief Create the address space, map the push buffer and the semaphore
     * into it, and create channel `id` on `engine` with `chip`'s classes and
     * `card`'s system memory.
     * @return `OK`, `ERR_BAD_STATE` while the channel or its address space
     * exists, `ERR_NO_MEMORY`, or the error of the address space, a mapping or
     * the channel, logged but for `ERR_BAD_STATE`, the channel's
     * `ERR_INVALID_ARG` and an `ERR_GPU_FAILED`. On an error what the firmware
     * or the GPU may still reach stays for good, and the rest is released.
     */
    int32_t create(const dma::device_profile& card, const chip_info& chip, const engine_info& engine, uint32_t id);

    /**
     * @brief Destroy the channel, unmap and free the push buffer and the
     * semaphore, then destroy the address space.
     * @return `OK`, `ERR_BAD_STATE` before `create`, or the first error of
     * the channel, the unmap or the address space. Every error but
     * `ERR_BAD_STATE` and the address space's `ERR_GPU_FAILED` is logged.
     * After the channel's destroy fails, the push buffer, the semaphore and
     * the address space stay for good.
     */
    int32_t destroy();

    /**
     * @brief Have the copy engine write `value` to every 4-byte element of
     * the `bytes` of video memory at `offset`, and wait until it has. While
     * `COPY_SLOTS - 1` fills run, it first waits for the oldest to finish.
     * @return `OK`, `ERR_INVALID_ARG` for a range `write_fill` cannot take,
     * `ERR_BAD_STATE` before `create`, or `ERR_TIMEOUT` once the channel is
     * hung. Only the `ERR_TIMEOUT` of the fill that marks it hung is logged.
     */
    int32_t fill(uint64_t offset, uint64_t bytes, uint32_t value);

    /**
     * @brief Read the semaphore and wake every waiting fill, from the card's
     * interrupt handler after it clears the pending bit of the copy engine's
     * nonstall vector. Does nothing before `create`. Takes the channel's
     * spinlock and never sleeps.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void service();

private:
    friend struct copy_channel_test_access;

    /**
     * @brief Submit the fill once fewer than `COPY_SLOTS - 1` fills run, then
     * wait for it, marking the channel hung once the oldest running fill passes
     * the deadline.
     * @return Whether the fill finished.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool run_fill(uint64_t offset, uint64_t bytes, uint32_t value);

    // Writes fill `sequence` and its release sequence into its slot and has the GPU fetch them, starting the deadline
    // when no other fill runs, under the lock
    void submit(uint64_t sequence, uint64_t offset, uint64_t bytes, uint32_t value);

    // Moves the completed count up to the semaphore's value, restarting the deadline while fills run, under the lock
    void read_progress();

    // Whether a fill runs and `now_ns` has reached the deadline, under the lock. `now_ns` must be read before the
    // semaphore, or a release landing before the deadline could be missed and its fill counted as hung
    bool is_past_deadline(uint64_t now_ns) const;

    // Unmaps the push buffer and the semaphore and frees them, or keeps them for good when `gpu_may_reach` is set
    int32_t release_memory(bool gpu_may_reach);

    address_space m_space;
    channel       m_channel;

    dma::buffer m_memory = {}; // the push buffer's page, then the semaphore's
    uint32_t    m_copy_class = 0;
    uint64_t    m_fill_timeout_ns = GPU_SUBMIT_TIMEOUT_NS; // how long the oldest running fill may take
    bool        m_is_mapped = false;

    // Written under the lock and read bare by `fill`, which callers keep from running during `create` or `destroy`
    bool m_is_created = false;

    sync::spinlock   m_lock = sync::SPINLOCK_INIT;
    sync::wait_queue m_waiters;         // fills waiting for a free slot or for their copy
    uint64_t         m_submitted = 0;   // the sequence number of the last fill submitted
    uint64_t         m_completed = 0;   // the sequence number of the last fill the semaphore showed done
    uint64_t         m_deadline_ns = 0; // when the oldest running fill counts as hung
    bool             m_is_hung = false;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_COPY_CHANNEL_H
