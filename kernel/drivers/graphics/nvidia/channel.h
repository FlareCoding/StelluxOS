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

// NVIDIA's NV_CHRAM_CHANNEL(i), a channel's word in its runlist's channel RAM, 4 bytes per id
constexpr uint32_t CHANNEL_RAM_ENTRY_BYTES = 4;

// The queues a group's channels can be fetched on, each with a buffer of the group's for a faulting channel's methods
constexpr uint32_t CHANNEL_GROUP_RUNQUEUES = 2;

// The objects a channel holds at most, a graphics queue's 3D, compute and copy objects
constexpr uint32_t CHANNEL_OBJECTS_MAX = 3;

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
 * together), the channel in it, the objects the channel's methods address, and
 * the memory the driver allocates for them: the instance block (the GPU's
 * record of the channel, in video memory), and in system memory USERD (where
 * the driver publishes how far the ring is written), the GPFIFO and the group's
 * fault method buffers (where an engine saves a faulting channel's methods).
 *
 * Every call but `write_entry`, `kick` and the accessors waits for the firmware
 * or for video memory, so none may run in an interrupt handler or under a
 * spinlock. Callers must not overlap `write_entry` and `kick`. After a release
 * or the GPFIFO's unmap fails, the memory stays allocated for good, since the
 * firmware or the GPU may still reach it.
 */
class channel {
public:
    // An object holding nothing, using `fw`, `objects`, `memory` and `bar0`, which must outlive it
    channel(firmware& fw, const gpu_objects& objects, video_memory& memory, registers bar0);

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    /**
     * @brief Create a copy channel: `allocate` the channel, add a copy object
     * of `chip`'s class on `setup`'s engine, then `schedule` the channel.
     * @return `OK`, or the first error of the three, after which the object
     * holds nothing, as after `destroy`, unless `allocate` found the channel
     * existing.
     */
    int32_t create(
        const dma::device_profile& card,
        const chip_info& chip,
        address_space& space,
        const channel_setup& setup
    );

    /**
     * @brief Allocate the channel's memory, map its GPFIFO read-only into
     * `space` and allocate its group and channel as `setup` says, with no
     * object, unbound and unscheduled.
     * @return `OK`, `ERR_BAD_STATE` while the channel exists, `ERR_INVALID_ARG`
     * for an id from `CHANNEL_IDS` on, or the failing step's error. The caller
     * logs those two, the mapping's `ERR_GPU_FAILED` and no room in video
     * memory, system memory or BAR1's window, and this logs the rest. On an
     * error but `ERR_BAD_STATE` the object holds nothing.
     */
    int32_t allocate(
        const dma::device_profile& card,
        const chip_info& chip,
        address_space& space,
        const channel_setup& setup
    );

    /**
     * @brief Allocate an object of `object_class` on the channel from `bytes`
     * of `params`, which may be null when `bytes` is 0. `destroy` releases the
     * objects before the channel, the last added first.
     * @return `OK`, `ERR_BAD_STATE` before `allocate` or once the channel holds
     * `CHANNEL_OBJECTS_MAX` objects, or the firmware's error, logged.
     */
    int32_t add_object(uint32_t object_class, void* params, size_t bytes);

    // Adds a copy object of `chip`'s class for the copy engine of type `engine_type`, as `add_object` does
    int32_t add_copy_object(const chip_info& chip, uint32_t engine_type);

    /**
     * @brief Bind the channel to its engine, then schedule it, so the GPU
     * fetches the entries `kick` publishes.
     * @return `OK`, `ERR_BAD_STATE` before `allocate`, or the firmware's error,
     * logged. On an error the channel stays allocated, bound if only the
     * schedule failed, for `destroy`.
     */
    int32_t schedule();

    /**
     * @brief Release the channel's objects, the channel and the group, then
     * unmap the GPFIFO and release the memory, as the class says.
     * @return `OK`, `ERR_BAD_STATE` before `allocate`, or the first error of a
     * release or of the unmap. Every error but `ERR_BAD_STATE` is logged. The
     * object then holds nothing either way.
     */
    int32_t destroy();

    /**
     * @brief Release the channel's objects, the channel and the group as
     * `destroy` does, so the GPU runs none of the channel's work once they are
     * released, and keep the memory for good. The object must not be created or
     * allocated again.
     * @return `OK`, `ERR_BAD_STATE` before `allocate`, or the first error of a
     * release, logged. The GPFIFO and USERD stay mapped, since a `write_entry`
     * or `kick` already under way may still write them. Such a `kick` rings the
     * freed channel's doorbell, so its id must go to no other channel.
     */
    int32_t destroy_keeping_memory();

    // Writes `entry` into the ring's next slot, which must hold no entry the GPU still has to fetch
    void write_entry(const gpfifo_entry& entry);

    // Has the GPU fetch every entry written: moves USERD's put index past them, then rings the doorbell
    void kick();

    // The channel's id in its runlist, valid while the channel exists
    uint32_t id() const;

    // The firmware's handle of the channel, which a control naming the channel gives, or 0 while none is held
    rm::handle handle() const;

    // Whether its runlist's channel RAM marks the channel faulted in fetching or running its work, valid while the
    // channel exists. A word of all ones, as a card that stopped responding reads, marks no fault
    bool has_faulted() const;

    // Whether a failed `create` or `allocate`, the last `destroy` or `destroy_keeping_memory` left the memory
    // allocated for good
    bool keeps_memory() const;

private:
    friend struct channel_test_access;

    // Allocates the instance block, cleared, then USERD, the GPFIFO and the fault method buffers
    int32_t allocate_memory(const dma::device_profile& card);

    // Allocates the group, then the channel in it
    int32_t allocate_channel(const chip_info& chip, address_space& space, const channel_setup& setup);

    // Releases the channel's objects, the last added first, then the channel and the group, returning the first error
    int32_t release_objects();

    // Releases `object` unless it is 0, keeping the memory on a refusal, and sets it to 0. Returns `first_rc` unless
    // it is `OK`, else the release's result
    int32_t release_object(rm::handle& object, int32_t first_rc);

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
    rm::handle m_objects_held[CHANNEL_OBJECTS_MAX] = {};
    uint32_t   m_object_count = 0;

    uint32_t m_id = 0;
    uint32_t m_engine_type = 0;
    uint32_t m_doorbell_token = 0;
    uint32_t m_channel_ram_entry = 0; // the BAR0 offset of the channel's word in its runlist's channel RAM
    uint32_t m_put = 0;               // the slot the next entry goes in
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHANNEL_H
