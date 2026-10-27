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

/**
 * The display's instance memory, the video memory where the display engine
 * looks up the memory its channels (the queues of commands it reads from
 * memory) may read: a hash table from a client, an object's handle and a
 * channel to the object's record, which names a range of memory. Once the
 * GSP's firmware is told the memory's address, it writes the address into the
 * display engine's registers, and the display engine may read the memory from
 * then on, so the object keeps the memory for the driver's life. Its calls
 * must come from one thread at a time, since the object takes no lock, and
 * `set_up` waits on video memory's lock, on the card and on the firmware's
 * reply, so it must never run in an interrupt handler or under a spinlock, and
 * on a thread that cannot sleep, such as the boot thread, only while no other
 * thread can call into video memory or the firmware.
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

private:
    video_memory& m_memory;
    firmware&     m_fw;
    vram_range    m_range;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_INSTANCE_MEMORY_H
