// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHANNEL_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHANNEL_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/address_space.h"
#include "drivers/graphics/nvidia/channel_methods.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/context_page_tables.h"
#include "drivers/graphics/nvidia/engine_table.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"

namespace drivers::nvidia {

// The entries of a channel's ring of work, and the bytes the ring maps in the channel's address space
constexpr uint32_t GPFIFO_ENTRIES = 16384;
constexpr uint64_t GPFIFO_BYTES   = static_cast<uint64_t>(GPFIFO_ENTRIES) * GPFIFO_ENTRY_BYTES;

// The ids a channel's allocation can give a channel in its runlist
constexpr uint32_t CHANNEL_IDS = 4096;

// The queues a group's channels can be fetched on, each with a buffer of the group's for a faulting channel's methods
constexpr uint32_t CHANNEL_GROUP_RUNQUEUES = 2;

// Where a channel runs and how far the GPU trusts it
struct channel_setup {
    engine_info engine;         // the engine running its work, on that engine's runlist
    uint32_t    id;             // its id in the runlist, below `CHANNEL_IDS` and held by no other channel there
    uint64_t    gpfifo_address; // where its GPFIFO maps in its address space, a multiple of 4 KiB
    bool        is_privileged;  // whether it may copy by physical address and issue the privileged MEM_OPs
};

/**
 * A channel: a queue of work for one engine. Its GPFIFO is a ring of entries,
 * each pointing the GPU at method words in the channel's address space. The
 * object holds a channel group of its own (the channels the scheduler runs
 * together), the channel in it, the copy object the channel's methods address,
 * and the memory the driver allocates for them: the instance block (the GPU's
 * record of the channel, in video memory), and in system memory USERD (where
 * the driver publishes how far the ring is written), the GPFIFO and the group's
 * fault method buffers (where an engine saves a faulting channel's methods).
 *
 * `create` and `destroy` wait for the firmware and for video memory, so they
 * must not run in an interrupt handler or under a spinlock. Callers must not
 * overlap `write_entry` and `kick`. After a release or the GPFIFO's unmap
 * fails, the memory stays allocated for good, since the firmware or the GPU may
 * still reach it.
 */
class channel {
public:
    // An object holding nothing, using `fw`, `objects`, `memory` and `bar0`, which must outlive it
    channel(firmware& fw, const gpu_objects& objects, video_memory& memory, registers bar0);

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    /**
     * @brief Allocate the channel's memory, its system memory through `card`,
     * map the GPFIFO read-only into `space`, allocate the group, then the
     * channel and its copy object from `chip`'s classes, then bind and schedule
     * the channel as `setup` says.
     * @return `OK`, `ERR_BAD_STATE` while the channel exists, `ERR_INVALID_ARG`
     * for an id from `CHANNEL_IDS` on, `ERR_NO_MEMORY`, `ERR_BAD_MESSAGE` for a
     * fault method buffer size of 0, or the error of video memory, the mapping
     * or the firmware. Every error is logged but `ERR_BAD_STATE`, the id's
     * `ERR_INVALID_ARG`, the mapping's `ERR_GPU_FAILED` and no room in video
     * memory, system memory or BAR1's window, which the caller logs.
     */
    int32_t create(
        const dma::device_profile& card,
        const chip_info& chip,
        address_space& space,
        const channel_setup& setup
    );

    /**
     * @brief Release the copy object, the channel and the group, then unmap
     * the GPFIFO and release the memory, as the class says.
     * @return `OK`, `ERR_BAD_STATE` before `create`, or the first error of a
     * release or of the unmap. Every error but `ERR_BAD_STATE` is logged. The
     * object then holds nothing either way.
     */
    int32_t destroy();

    // Writes `entry` into the ring's next slot, which must hold no entry the GPU still has to fetch
    void write_entry(const gpfifo_entry& entry);

    // Has the GPU fetch every entry written: moves USERD's put index past them, then rings the doorbell
    void kick();

    // The channel's id in its runlist, valid while the channel exists
    uint32_t id() const;

    // Whether a failed `create` or the last `destroy` left the channel's memory allocated for good, as the class says
    bool keeps_memory() const;

private:
    friend struct channel_test_access;

    // Allocates the instance block, cleared, then USERD, the GPFIFO and the fault method buffers
    int32_t allocate_memory(const dma::device_profile& card);

    // Allocates the group, the channel and its copy object, then binds and schedules the channel
    int32_t allocate_objects(const chip_info& chip, address_space& space, const channel_setup& setup);

    // Releases the objects that exist, the last allocated first, returning the first error
    int32_t release_objects();

    // Unmaps the GPFIFO and releases the memory, or forgets the memory the firmware or the GPU may still reach,
    // returning the unmap's error
    int32_t release_memory();

    firmware&          m_firmware;
    const gpu_objects& m_objects;
    video_memory&      m_video_memory;
    registers          m_bar0;

    vram_range           m_instance_block = {};
    dma::buffer          m_userd = {};
    dma::buffer          m_gpfifo = {};
    dma::buffer          m_method_buffers[CHANNEL_GROUP_RUNQUEUES] = {};
    uint32_t             m_method_buffer_bytes = 0;
    context_page_tables* m_gpfifo_tables = nullptr; // the tables mapping the GPFIFO, null when the object holds none
    uint64_t             m_gpfifo_address = 0;
    bool                 m_keeps_memory = false;    // the firmware or the GPU may still reach the memory

    rm::handle m_group = 0;
    rm::handle m_channel = 0;
    rm::handle m_copy_object = 0;

    uint32_t m_id = 0;
    uint32_t m_doorbell_token = 0;
    uint32_t m_put = 0; // the slot the next entry goes in
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHANNEL_H
