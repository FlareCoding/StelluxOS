// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_VRAM_ALLOCATOR_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_VRAM_ALLOCATOR_H

#include "common/list.h"
#include "common/types.h"
#include "drivers/graphics/nvidia/gsp_info.h"
#include "sync/mutex.h"

namespace drivers::nvidia {

// What the card does with an allocation, the use that decides the video memory regions the allocation may lie in
enum class vram_use : uint8_t {
    general, // read or written by the card's engines, the display engine's channels or the CPU, never shown on screen
    scanout, // an image the display engine sends to the screen, a window's surface or a cursor's picture
};

// A range of video memory `vram_allocator` hands out
struct vram_range {
    uint64_t offset; // from the start of video memory, a multiple of 4 KiB
    uint64_t size;   // bytes, a multiple of 4 KiB
};

/**
 * Hands out ranges of the card's video memory and takes them back. The GSP's
 * firmware reports video memory as regions, each with flags saying what may
 * lie in it and a performance number, higher for faster memory, as
 * `video_memory_region` holds them. A reserved region holds memory the
 * firmware keeps or may need, such as the boot console's, and every other
 * region is usable. The allocator hands out ranges of usable regions only,
 * each lying wholly inside one region, as NVIDIA's heap keeps inside one
 * region each contiguous allocation whose offset it chooses.
 *
 * Each allocation names its use, and the allocator alone turns a use into the
 * regions the memory may lie in, by the regions' flags as NVIDIA's heap
 * applies them:
 *
 *   use       the usable regions it may lie in
 *   general   those not for protected allocations
 *   scanout   those not for protected allocations that support scanout
 *
 * A region for protected allocations holds only allocations NVIDIA's driver
 * marks protected, and no use is protected. Compression support, whether the
 * card may store memory in the region compressed, decides nothing, since
 * NVIDIA's heap restricts only allocations that require compression, and no
 * use does. The regions a use may lie in are tried highest performance first,
 * as NVIDIA's heap tries them when it chooses a contiguous allocation's
 * offset. Regions of equal performance are tried in the firmware's order,
 * which NVIDIA's exchange sort can reverse for two of them. Inside a region,
 * the lowest free offset that meets the alignment wins, while NVIDIA's heap
 * places some allocations at the top of the highest free block that fits, by
 * its placement strategy for their type or by flags its caller passes.
 * NVIDIA's heap also tries regions slowest first for allocations of low
 * priority, and no use is of low priority.
 *
 * The allocator keeps one record per allocation, sorted by offset, on the
 * unprivileged heap, and frees the records left when it is destroyed. It
 * touches no hardware. A call that passes its checks takes the allocator's
 * mutex, elevating for it, so a call can block: it must never run in an
 * interrupt handler or under a spinlock, and a thread that cannot sleep, such
 * as the boot thread, must call only while no other thread can. A range
 * handed out is its caller's until the caller releases it.
 */
class vram_allocator {
public:
    /**
     * @brief An allocator with no regions, whose `allocate` fails until
     * `init` succeeds.
     */
    vram_allocator();

    /**
     * @brief Free the record of every allocation not released.
     */
    ~vram_allocator();

    vram_allocator(const vram_allocator&) = delete;
    vram_allocator& operator=(const vram_allocator&) = delete;

    /**
     * @brief Take the video memory regions of `configuration` for the
     * allocations, once they pass three checks of this driver's own, which
     * NVIDIA's driver does not make: no region overlaps another, every usable
     * region ends at or below `video_memory_size`, and the first entry of
     * BAR1's root page directory, which bring-up rewrites when it keeps a boot
     * console at the start of BAR1, lies wholly inside a reserved region.
     * Otherwise the allocator could hand out a byte twice, a reserved byte, a
     * byte the card lacks or that entry, and it trusts no part of a table that
     * fails a check. A reserved region may end past `video_memory_size`, since
     * no range is handed out of it. The regions' count and sizes are taken as
     * `read_static_configuration` checked them. It must be called at most once,
     * since a second call would replace the regions of the ranges already
     * handed out.
     * @param configuration The static configuration `read_static_configuration`
     * read.
     * @param video_memory_size The video memory's size in bytes, as
     * `frts_region::video_memory_size` holds it.
     * @return `OK`, or `ERR_BAD_MESSAGE`, logged with the check that failed,
     * leaving the allocator's regions as they were.
     */
    int32_t init(const static_configuration& configuration, uint64_t video_memory_size);

    /**
     * @brief Hand out `size` bytes of video memory for `use` at a multiple of
     * `alignment`, in the first region `use` may lie in, in the order above,
     * with a free range of that size at that alignment.
     * @param size The bytes needed, rounded up to a multiple of 4 KiB, the
     * GPU's small page.
     * @param alignment A power of two, any below 4 KiB counting as 4 KiB.
     * @param use What the card does with the memory.
     * @param out Receives the range on `OK`, its size the rounded size, and is
     * left unchanged on every error.
     * @return `OK`, `ERR_INVALID_ARG` when `size` is 0 or overflows when
     * rounded and when `alignment` is 0 or not a power of two, or
     * `ERR_NO_MEMORY` when no region `use` may lie in has a free range of that
     * size at that alignment, when the record cannot be allocated and before
     * `init` succeeds. Refused arguments and a failure to allocate the record
     * are logged.
     */
    int32_t allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out);

    /**
     * @brief Take back a range `allocate` handed out, whole and once, freeing
     * its record. Any other range, such as one released already or part of an
     * allocation, is logged and changes nothing, so a release cannot fail. A
     * release allocates nothing.
     * @param range The range as `allocate` handed it out.
     */
    void release(const vram_range& range);

private:
    // The record of one allocation
    struct record {
        uint64_t   offset;
        uint64_t   size;
        list::node link;
    };

    // Places and records `size` bytes for `use` at a multiple of `alignment`, both whole pages, with the mutex held
    int32_t place_range(uint64_t size, uint64_t alignment, vram_use use, vram_range& out);

    // The lowest offset in `region` at a multiple of `alignment` where `size` bytes are free, with the mutex held
    bool find_free_offset(const video_memory_region& region, uint64_t size, uint64_t alignment, uint64_t& offset_out);

    // The record of an allocation of exactly `range`, or null, with the mutex held
    record* find_record(const vram_range& range);

    video_memory_region               m_regions[VIDEO_MEMORY_REGIONS_MAX]; // the regions allocations may lie in, in the
                                                                           // order `allocate` tries them
    uint32_t                          m_region_count = 0;
    list::head<record, &record::link> m_records;                           // sorted by offset
    sync::mutex                       m_lock;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_VRAM_ALLOCATOR_H
