#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GRAPHICS_CONTEXT_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GRAPHICS_CONTEXT_H

#include "common/types.h"
#include "drivers/graphics/nvidia/firmware.h"

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

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GRAPHICS_CONTEXT_H
