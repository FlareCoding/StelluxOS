// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_INSTANCE_MEMORY_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_INSTANCE_MEMORY_H

#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/graphics/nvidia/vram_allocator.h"
#include "common/types.h"

namespace drivers::nvidia {

// NVIDIA's size of the display's instance memory, which `instance_memory` describes: an 8 KiB hash table, then 56 KiB
// for records
constexpr uint64_t INSTANCE_MEMORY_BYTES = 0x10000;

// NVIDIA's DISP_INST_MEM_ALIGN, the alignment of the display's instance memory in video memory
constexpr uint64_t INSTANCE_MEMORY_ALIGNMENT = 0x10000;

// How many entries the hash table holds, 8 bytes each in its 8 KiB, and how many 32-byte records follow it in 56 KiB,
// skipping the first 32 bytes, where NVIDIA's driver reserves 16 bytes as its NULL instance, which no context DMA gets
constexpr uint32_t INSTANCE_MEMORY_HASH_ENTRIES = 1024;
constexpr uint32_t INSTANCE_MEMORY_RECORDS      = 1791;

// NVIDIA's display channel numbers run from 0 to 80: the core channel 0, window i's channel 1 + i and its immediate
// channel 33 + i for 32 windows, and cursor i's channel 73 + i for 8 cursors, while 65 to 72 name no channel
constexpr uint32_t DISPLAY_CHANNEL_NUMBERS = 81;

// The memory a context DMA names, starting on a 256-byte boundary
struct context_dma_memory {
    bool     in_video_memory; // video memory, else DMA pages read with snooping on
    uint64_t address;         // the offset into video memory, or the pages' physical address
    uint64_t bytes;           // nonzero
};

/**
 * The display's instance memory, the video memory where the display engine
 * looks up the memory its channels (the queues of commands it reads from
 * memory) may read: a hash table from a client, an object's handle and a
 * channel to the object's record, which names a range of memory. Once the
 * GSP's firmware is told the memory's address, it writes the address into the
 * display engine's registers, and the display engine may read the memory from
 * then on, so the object keeps the memory for the driver's life.
 *
 * The objects the records describe are context DMAs, and a channel may name a
 * context DMA once `bind` has written the record and the channel's hash table
 * entry for it, until `unbind` clears the entry. A context DMA bound to
 * several channels has one record, kept until its last binding is undone. So a
 * caller must undo a binding only once no method still to run on the channel
 * names the context DMA, and its last binding only once the display engine no
 * longer reads the memory it names, since a later binding may rewrite the
 * freed record. This object keeps its own copy of the entries and of how many
 * bindings share each record, as NVIDIA's driver does, so finding an entry
 * reads no video memory, and `bind` and `unbind` map the memory for the CPU
 * only while they write, since `video_memory::unmap_for_cpu` is what completes
 * the writes.
 *
 * The object's calls must come from one thread at a time, since it takes no
 * lock, and `set_up`, `bind` and `unbind` wait on video memory's lock and on
 * the card, `set_up` also on the firmware's reply, so they must never run in
 * an interrupt handler or under a spinlock, and on a thread that cannot sleep,
 * such as the boot thread, must run only while no other thread can call into
 * video memory or the firmware.
 */
class instance_memory {
public:
    /**
     * @brief An object holding no memory until `set_up` takes the instance
     * memory from `memory` and tells `fw` its address.
     * @param memory The driver's video memory, which must outlive the object.
     * @param fw The firmware the driver's requests go to, which must outlive
     * the object.
     */
    instance_memory(video_memory& memory, firmware& fw);

    instance_memory(const instance_memory&) = delete;
    instance_memory& operator=(const instance_memory&) = delete;

    /**
     * @brief Allocate `INSTANCE_MEMORY_BYTES` of `vram_use::general` video
     * memory at a multiple of `INSTANCE_MEMORY_ALIGNMENT`, clear it through a
     * mapping for the CPU, which `video_memory::unmap_for_cpu` takes down
     * after completing the writes through it, and send its address to the
     * firmware with `rm::CONTROL_WRITE_DISPLAY_INSTANCE_MEMORY` through
     * `firmware::internal_control`, marked as video memory the CPU maps
     * write-combined, as NVIDIA's driver marks it when the firmware runs on
     * the GSP. The clear is this driver's own step, since NVIDIA's driver
     * writes nothing into the memory before sending its address. It must be
     * called before the driver allocates any display object, as NVIDIA's
     * driver sets the memory up at its display's state init, and at most once,
     * since a second call would leave the first memory allocated and give the
     * firmware the second's address.
     * @return `OK`, or what `video_memory::allocate`,
     * `video_memory::map_for_cpu` or `firmware::internal_control` returns,
     * each logged here. After a failed mapping the memory is released. After a
     * failed control it is kept whatever the error, since the firmware may
     * hold its address once the request is sent, and an error before sending
     * wastes at most its 64 KiB until the driver is freed.
     */
    int32_t set_up();

    /**
     * @brief The instance memory's range of video memory, of 0 bytes until
     * `set_up` has cleared it.
     */
    const vram_range& range() const;

    /**
     * @brief Bind the context DMA `object`, under the driver's client, to the
     * display engine's channel `hardware_channel`, as NVIDIA's driver binds
     * one: a first binding, made while the context DMA has no other, writes
     * its record, naming `memory` as rows of pixels in order (pitch) and
     * readable and writable, into the lowest free record, and each binding
     * writes the entry naming the client, `object`, the channel and the record
     * into the first free entry from the hash of the three. What it writes
     * goes through a mapping for the CPU that `video_memory::unmap_for_cpu`
     * takes down after completing the writes, so they have reached video
     * memory when the call returns.
     * @param object The context DMA's handle, nonzero.
     * @param hardware_channel The channel's number, 0 to 64 or 73 to 80, the
     * numbers that name a channel.
     * @param memory The memory a first binding writes into its record. It must
     * end within the first 128 TiB of its address space, since a record holds
     * only 39 bits of a 256-byte unit's number. `bind` checks a further
     * binding's `memory` too, and the record that binding shares keeps naming
     * the first binding's memory.
     * @return `OK`, `ERR_INVALID_ARG` when `object` is 0, `hardware_channel`
     * names no channel, or `memory` is not 256-byte aligned, has 0 bytes or
     * ends past 128 TiB, `ERR_BAD_STATE` when `set_up` has not cleared the
     * memory, the driver has no client or `object` is bound to
     * `hardware_channel` already, `ERR_NO_MEMORY` when the hash table has no
     * free entry, or what `video_memory::map_for_cpu` returns, each logged. On
     * every error the instance memory and the object's copy are unchanged.
     */
    int32_t bind(rm::handle object, uint32_t hardware_channel, const context_dma_memory& memory);

    /**
     * @brief Undo the binding of the context DMA `object` to the channel
     * `hardware_channel`, as NVIDIA's driver undoes one: clear its entry to
     * zeros through a mapping for the CPU that `video_memory::unmap_for_cpu`
     * takes down after completing the write, and free its record once no other
     * binding shares it. A freed record's words stay as they are until a later
     * binding takes the record, as NVIDIA's driver leaves them.
     * @param object The context DMA's handle.
     * @param hardware_channel The channel's number.
     * @return `OK`, `ERR_BAD_STATE` when `object` is not bound to
     * `hardware_channel`, as is always so for handle 0 and for a number that
     * names no channel, or what `video_memory::map_for_cpu` returns, each
     * logged. On every error the instance memory and the object's copy are
     * unchanged, so a failed mapping keeps the binding and the record its
     * entry still names, whereas NVIDIA's driver drops the binding from its
     * copy and its count even when writing the entry fails.
     */
    int32_t unbind(rm::handle object, uint32_t hardware_channel);

private:
    // The object's copy of one hash table entry: the context DMA it binds, 0 while the entry is free, the record and
    // the channel
    struct binding {
        rm::handle object;
        uint16_t   record;
        uint8_t    hardware_channel;
    };

    // The entry binding `object` to `hardware_channel`, or `INSTANCE_MEMORY_HASH_ENTRIES` when there is none
    uint32_t find_binding(rm::handle object, uint32_t hardware_channel) const;

    // The first free entry from `hash` on, wrapping past the last, or `INSTANCE_MEMORY_HASH_ENTRIES` when all are taken
    uint32_t find_free_entry(uint32_t hash) const;

    // The record `object`'s bindings share, or `INSTANCE_MEMORY_RECORDS` when it has none
    uint32_t find_record(rm::handle object) const;

    // The lowest record no binding uses, which exists while an entry is free
    uint32_t find_free_record() const;

    video_memory& m_memory;
    firmware&     m_fw;
    vram_range    m_range;

    binding m_bindings[INSTANCE_MEMORY_HASH_ENTRIES]; // entry i of the hash table
    uint8_t m_record_users[INSTANCE_MEMORY_RECORDS];  // how many bindings share record i, 0 while it is free
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_INSTANCE_MEMORY_H
