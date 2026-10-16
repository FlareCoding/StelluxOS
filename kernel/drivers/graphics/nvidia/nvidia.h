#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H

#include "common/types.h"

namespace drivers::nvidia {

// Result codes shared by every part of the driver
constexpr int32_t OK                 = 0;
constexpr int32_t ERR_NOT_RESPONDING = -1; // `PMC_BOOT_0` reads all ones, so the card does not respond
constexpr int32_t ERR_BAD_VBIOS      = -2; // the VBIOS lacks a structure bring-up reads from it, or one is malformed
                                           // or ends past the ROM window or the VBIOS
constexpr int32_t ERR_BAD_FIRMWARE   = -3; // a firmware file is missing, unreadable, malformed, of another version or
                                           // lacks a signature bring-up needs, or a chip table name for it is invalid
constexpr int32_t ERR_TIMEOUT        = -4; // the card did not finish a step within its deadline
constexpr int32_t ERR_BAD_STATE      = -5; // the card's registers or the driver's records show a state bring-up cannot
                                           // proceed from, such as protected memory or boot memory already in place
constexpr int32_t ERR_NO_MEMORY      = -6; // allocating memory for the card to reach, or the list of the runs
                                           // holding the GSP's firmware, failed
constexpr int32_t ERR_QUEUE_FULL     = -7; // the command queue has no room for a message, as the read pointer `send`
                                           // reads for the firmware has not moved far enough or lies past the ring
constexpr int32_t ERR_TOO_LARGE      = -8; // a payload exceeds the most bytes a message to the GSP's firmware carries,
                                           // or the room a caller gives a received message's payload
constexpr int32_t ERR_NOT_LINKED     = -9; // the status queue is not linked, as no call to `link_status_queue` has
                                           // accepted its header, such as before the firmware writes it
constexpr int32_t ERR_QUEUE_EMPTY    = -10; // the status queue shows no message to read, as the firmware's write
                                            // pointer equals the read pointer or lies past the ring
constexpr int32_t ERR_BAD_MESSAGE    = -11; // a message from the GSP's firmware fails a check of its element count, the
                                            // length its RPC header gives, its checksum or its sequence number

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

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H
