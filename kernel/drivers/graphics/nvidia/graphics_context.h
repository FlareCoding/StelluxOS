#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GRAPHICS_CONTEXT_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GRAPHICS_CONTEXT_H

#include "common/types.h"
#include "drivers/graphics/nvidia/context_page_tables.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/vram_allocator.h"

namespace drivers::nvidia {

// A graphics context buffer's bytes and the multiple its video memory offset and its GPU address must be
struct context_buffer_layout {
    uint64_t bytes;
    uint64_t alignment;
};

/**
 * The graphics engine's context buffers, the memory it saves a channel's state
 * to and its units work in, as the firmware sizes them. Each graphics channel
 * has a main and a patch buffer of its own, and the rest are allocated once
 * and mapped into every address space holding a graphics channel.
 */
struct graphics_buffer_layouts {
    context_buffer_layout main; // a header for each subcontext a channel group can hold included
    context_buffer_layout patch;
    context_buffer_layout bundle;
    context_buffer_layout page_pool;
    context_buffer_layout attribute;
    context_buffer_layout rtv;
    context_buffer_layout fecs_event;
    context_buffer_layout priv_access_map; // each of the two register access maps
};

// A context buffer's video memory and the address it maps at in each address space mapping it
struct context_buffer {
    vram_range range;
    uint64_t   gpu_address;
};

// The context buffers each graphics channel has of its own
struct channel_context_buffers {
    context_buffer main;
    context_buffer patch;
};

/**
 * The context buffers every graphics channel shares, allocated once.
 */
struct global_context_buffers {
    context_buffer bundle;
    context_buffer page_pool;
    context_buffer attribute;
    context_buffer rtv;
    context_buffer fecs_event;
    context_buffer priv_access_map;              // the map of an unprivileged channel, mapped read-only
    context_buffer unrestricted_priv_access_map; // the map of an administrator's channel, never mapped
};

/**
 * @brief Read into `layouts` the sizes and alignments of GR0's context
 * buffers, the main buffer rounded up to 4 KiB and grown by a header for each
 * subcontext a channel group can hold, each fact asked for through `fw`'s
 * internal client.
 * @return `OK`, `ERR_NO_MEMORY` when the unprivileged heap cannot hold a
 * reply, a control's error, or `ERR_BAD_MESSAGE` for a buffer sized 0, one the
 * chip lacks, a fact out of place or a subcontext count of 0. Every error but
 * a control's is logged here, and `layouts` changes only on `OK`.
 */
int32_t read_graphics_buffer_layouts(firmware& fw, graphics_buffer_layouts& layouts);

/**
 * @brief Give each buffer of `global` its address from `base` on, each at a
 * multiple of its layout's alignment and of 4 KiB, the attribute buffer at a
 * multiple of its size rounded up to a power of two, as the graphics engine
 * needs, and the unrestricted map none.
 * @return The address past the last buffer placed.
 */
uint64_t place_global_buffers(const graphics_buffer_layouts& layouts, uint64_t base, global_context_buffers& global);

/**
 * @brief Give `own` the addresses of queue slot `slot`'s own buffers, the main
 * buffer first. The slots' buffers follow one another from `start`, each slot
 * taking the same span, so every buffer lies at a multiple of its alignment and
 * of 4 KiB.
 */
void place_own_buffers(
    const graphics_buffer_layouts& layouts,
    uint64_t start,
    uint32_t slot,
    channel_context_buffers& own
);

/**
 * @brief Map every buffer of `global` but the unrestricted map into `tables`
 * at its address, privileged, the register access map read-only.
 * @return `OK`, or the first error of `context_page_tables::map`, after which
 * the buffers mapped stay mapped.
 */
int32_t map_global_buffers(context_page_tables& tables, const global_context_buffers& global);

/**
 * @brief Map both buffers of `own` into `tables` at their addresses,
 * privileged.
 * @return `OK`, or the first error of `context_page_tables::map`, after which
 * the buffer mapped stays mapped.
 */
int32_t map_own_buffers(context_page_tables& tables, const channel_context_buffers& own);

/**
 * @brief Hand the firmware, through `fw`'s control on `subdevice`, the context
 * buffers of graphics channel `channel` as `map_global_buffers` and
 * `map_own_buffers` map them: `own` initialized, and with `initializes_global`
 * the FECS event buffer and both register access maps too, as the GPU's first
 * promotion must. Every initialized buffer must be zero.
 * @return `OK`, or the firmware's error, logged.
 */
int32_t promote_graphics_context(
    firmware& fw,
    rm::handle subdevice,
    rm::handle channel,
    const global_context_buffers& global,
    const channel_context_buffers& own,
    bool initializes_global
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GRAPHICS_CONTEXT_H
