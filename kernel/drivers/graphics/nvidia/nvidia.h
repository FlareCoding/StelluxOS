// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H

#include "common/types.h"
#include "clock/clock.h"

namespace drivers::nvidia {

// Result codes shared by every part of the driver
constexpr int32_t OK                 = 0;
constexpr int32_t ERR_NOT_RESPONDING = -1; // `PMC_BOOT_0` reads all ones, so the card does not respond
constexpr int32_t ERR_BAD_VBIOS      = -2; // the VBIOS lacks a structure bring-up reads from it, or one is malformed
                                           // or ends past the ROM window or the VBIOS
constexpr int32_t ERR_BAD_FIRMWARE   = -3; // a firmware file is missing, unreadable, malformed, of another version or
                                           // lacks a signature bring-up needs, or a chip table name for it is invalid
constexpr int32_t ERR_TIMEOUT        = -4; // the card did not finish a step within its deadline
constexpr int32_t ERR_BAD_STATE      = -5; // the card's registers or memory, or the driver's records, show bring-up
                                           // cannot proceed, or a falcon does not read as stopped after `stop_falcon`
                                           // resets it
constexpr int32_t ERR_NO_MEMORY      = -6; // allocating memory for the card to reach, the list of the runs holding the
                                           // GSP's firmware, room for a received message's payload, the arguments of a
                                           // request, a control's parameters or a copy of the console's tables, failed
constexpr int32_t ERR_QUEUE_FULL     = -7; // the command queue has no room for a message, as the read pointer `send`
                                           // reads for the firmware has not moved far enough or lies past the ring
constexpr int32_t ERR_TOO_LARGE      = -8; // a payload exceeds the most bytes a message to the GSP's firmware carries,
                                           // or the room a caller of `receive` gives the message's payload
constexpr int32_t ERR_NOT_LINKED     = -9; // the status queue is not linked, as no call to `link_status_queue` has
                                           // accepted its header, such as before the firmware writes it
constexpr int32_t ERR_QUEUE_EMPTY    = -10; // the status queue shows no message to read, as the firmware's write
                                            // pointer equals the read pointer or lies past the ring
constexpr int32_t ERR_BAD_MESSAGE    = -11; // a message from the GSP's firmware fails a check of its element count, the
                                            // length its RPC header gives, its checksum, sequence number or size, or a
                                            // CPU sequencer, video memory layout or kernel interrupt table it carries
constexpr int32_t ERR_FIRMWARE       = -12; // the GSP's firmware reports a failure through a ready message's or reply's
                                            // result or a control's status, or does not take the driver's VGX version,
                                            // as a reply shows by a private result other than 0 or another version

// Converts the timeouts the driver logs to milliseconds
constexpr uint64_t NS_PER_MS = 1000000;

// NVIDIA's default timeout for a GPU in graphics mode rather than compute mode
constexpr uint64_t DEFAULT_TIMEOUT_NS = 4000000000;

/**
 * @brief Whether the `bytes` bytes at `offset` lie inside the first `limit`
 * bytes, also when `offset + bytes` would overflow.
 */
inline bool lies_inside(uint64_t offset, uint64_t bytes, uint64_t limit) {
    return offset <= limit && bytes <= limit - offset;
}

/**
 * @brief The little-endian 32-bit value in the 4 bytes at `bytes`, read a byte
 * at a time, so `bytes` needs no alignment.
 */
inline uint32_t read_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
           static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

/**
 * @brief Write `value` little-endian into the 4 bytes at `bytes` a byte at a
 * time, as `read_u32` reads them, so `bytes` needs no alignment.
 */
inline void write_u32(uint8_t* bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

/**
 * @brief The time left until `deadline_ns`, a time as `clock::now_ns` reads
 * it, or 0 once it has passed, so a wait given the result still reads once.
 */
inline uint64_t time_left_until(uint64_t deadline_ns) {
    uint64_t now = clock::now_ns();
    return deadline_ns > now ? deadline_ns - now : 0;
}

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H
