// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_INFO_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_INFO_H

#include "common/types.h"
#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// NVIDIA's numbers for the two requests its driver sends first once the GSP's firmware is ready, in the order it sends
// them, each reply carrying its request's number
constexpr uint32_t FUNCTION_SET_GUEST_SYSTEM_INFO = 0x01;
constexpr uint32_t FUNCTION_GET_GSP_STATIC_INFO   = 0x41;

// The VGX version (NVIDIA's number for the version of the messages between its driver and the firmware) that every
// message this driver builds follows, as NVIDIA's driver builds them for this firmware
constexpr uint32_t VGX_VERSION_MAJOR = 0x23;
constexpr uint32_t VGX_VERSION_MINOR = 0x05;

// The most video memory regions NVIDIA's driver keeps, the number of entries in NVIDIA's table of them
constexpr uint32_t VIDEO_MEMORY_REGIONS_MAX = 16;

// A range of video memory the GSP's firmware reports, with what NVIDIA's driver may place in it
struct video_memory_region {
    uint64_t offset;               // the region's first byte, counted from the start of video memory
    uint64_t size;                 // the region's bytes, from its first byte to the last the firmware gives
    uint64_t reserved_bytes;       // bytes in it the firmware leaves alone for a boot console or may need, which a
                                   // driver managing the memory leaves free
    bool     is_reserved;          // set when it has reserved bytes, as NVIDIA's driver then marks the whole region
                                   // reserved and counts none of it as usable
    bool     is_protected;         // only allocations marked protected may lie in it, and those lie nowhere else
    bool     supports_scanout;     // allocations of primary, cursor and video surfaces, which the display engine reads,
                                   // may lie in it, and only in such regions
    bool     supports_compression; // allocations that require compression may lie in it, and only in such regions
};

// The members of the GSP firmware's static configuration that this driver reads
struct static_configuration {
    video_memory_region regions[VIDEO_MEMORY_REGIONS_MAX]; // the video memory regions, in the firmware's order
    uint32_t            region_count;                      // how many entries of `regions` hold a region
    uint64_t            bar1_page_directory_offset;        // bytes from the start of video memory to the root page
                                                           // directory of the page tables the firmware binds BAR1 to
    rm::handle          internal_client;                   // the client the firmware keeps for the controls NVIDIA's
                                                           // driver sends on its own behalf
    rm::handle          internal_subdevice;                // that client's subdevice (its object standing for this
                                                           // GPU), to which the driver sends its first control
};

/**
 * @brief Tell the GSP's firmware which version of NVIDIA's messages this
 * driver builds, VGX version 0x23.05, and check that the firmware accepts it,
 * as NVIDIA's driver does with the first request it sends once the firmware is
 * ready. The request goes through `gsp::request` and carries the version, the
 * room of 256 bytes NVIDIA gives each of the three strings that follow and, as
 * the driver's version, the firmware's version string, which equals the one
 * NVIDIA's driver sends as its own. NVIDIA's branch name, title and changelist
 * number name its own build, so they are sent empty and 0. The firmware
 * accepts the version when the reply's private result is NVIDIA's success
 * value and the reply's copy of the arguments gives the version sent. NVIDIA's
 * driver reads the version whatever the reply's length, while here the payload
 * must hold both version fields, since the arguments past a shorter payload
 * keep the version sent. NVIDIA's driver also accepts another version a
 * successful reply gives, building its later messages for that version, and
 * sends the request again at the version a refusing reply gives when that
 * version differs from the one sent and is at least 0x18.00, while this driver
 * builds its messages for 0x23.05 alone, so either reply fails the call. The
 * call spins on the calling CPU without sleeping, through the wait for the
 * reply, which `gsp::request` bounds.
 * @param conversation The driver's `gsp`, whose status queue must be linked.
 * @param timeout_ns How long the wait for the reply may take, as
 * `gsp::request` takes it, `RECEIVE_TIMEOUT_NS` at bring-up.
 * @return `OK` once the firmware accepts the version, what `gsp::request`
 * returns when the request fails, logged as its doc describes, `ERR_FIRMWARE`
 * when the reply refuses the version or gives another, or `ERR_BAD_MESSAGE`
 * when a reply that does not refuse it is too short to give the version, both
 * logged.
 */
int32_t confirm_message_version(gsp& conversation, uint64_t timeout_ns);

/**
 * @brief Ask the GSP's firmware for its static configuration (the facts about
 * the GPU the firmware gathers as it starts), as NVIDIA's driver does with its
 * second request once the firmware is ready, and read from it the members
 * `static_configuration` holds: the video memory regions the firmware reports,
 * where BAR1's root page directory lies, and the handles of the internal
 * client and subdevice. BAR1 is the CPU's window into the card's video memory,
 * and its root page directory is the top level of the page tables the firmware
 * binds BAR1 to. The request carries 2168 zero bytes, the size of NVIDIA's
 * static configuration for this firmware, from a buffer allocated on the
 * unprivileged heap for the call, and `gsp::request` copies the reply's
 * payload back over those bytes. NVIDIA's driver checks only the reply's
 * result before copying the whole configuration, while here the reply's
 * payload must also extend to byte 2164, where the field holding the internal
 * subdevice's handle, the last field read, ends. The handles are taken as
 * given, as NVIDIA's driver takes them. The configuration's table of video
 * memory regions must give 1 to `VIDEO_MEMORY_REGIONS_MAX` regions, as
 * NVIDIA's driver requires. Each region's last byte must lie at or above its
 * first, and no region may span all 2^64 bytes, so that a region's size, its
 * last byte less its first plus 1 as NVIDIA counts it, never wraps. NVIDIA's
 * driver makes neither check. A region with reserved bytes is reserved, as
 * NVIDIA's driver decides when it reads the table. The root page directory's
 * offset must be a multiple of 8 (the size of its entries) below
 * `BAR0_WINDOW_REACH`, as `read_video_memory_u64` requires to read the first
 * entry, and that entry must end at or below `video_memory_size`, since an
 * offset past the card's memory names no page directory. NVIDIA's driver uses
 * the offset unchecked. Once the request succeeds, the call logs the reply's
 * payload size before reading the configuration. It spins on the calling CPU
 * without sleeping, through the wait for the reply, which `gsp::request`
 * bounds.
 * @param conversation The driver's `gsp`, whose status queue must be linked.
 * @param timeout_ns How long the wait for the reply may take, as
 * `gsp::request` takes it, `RECEIVE_TIMEOUT_NS` at bring-up.
 * @param video_memory_size The video memory's size in bytes, as
 * `frts_region::video_memory_size` holds it.
 * @param configuration Receives on `OK` the root page directory's offset (a
 * multiple of 8 below `BAR0_WINDOW_REACH`, with the directory's first entry
 * ending at or below `video_memory_size`), the regions in the firmware's order
 * with their count (1 to `VIDEO_MEMORY_REGIONS_MAX`), the entries past the
 * count left unchanged, and the internal client's and subdevice's handles. It
 * is left unchanged on an error.
 * @return `OK`, `ERR_NO_MEMORY` when the buffer cannot be allocated, what
 * `gsp::request` returns when the request fails, logged as its doc describes,
 * or `ERR_BAD_MESSAGE` when the reply's payload ends before byte 2164, when
 * the root page directory's offset is not a multiple of 8 below
 * `BAR0_WINDOW_REACH` or the directory's first entry ends past
 * `video_memory_size`, when the table gives no region or more than
 * `VIDEO_MEMORY_REGIONS_MAX`, or when a region's last byte lies below its
 * first or the region spans all 2^64 bytes. Every error but the request's is
 * logged here.
 */
int32_t read_static_configuration(
    gsp& conversation,
    uint64_t timeout_ns,
    uint64_t video_memory_size,
    static_configuration& configuration
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_INFO_H
