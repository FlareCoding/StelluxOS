#ifndef STELLUX_DRIVERS_SERIAL_SERIAL_H
#define STELLUX_DRIVERS_SERIAL_SERIAL_H

#include "common/types.h"

namespace serial {

// serial::uart gives -1 to ERR_NO_DEVICE, so each name keeps one value in both namespaces
constexpr int32_t OK              = 0;
constexpr int32_t ERR_UNSUPPORTED = -2;

enum class parity : uint8_t {
    none,
    odd,
    even,
    mark,  // parity bit always 1
    space, // parity bit always 0
};

/**
 * The speed, character format and flow control a serial port asks its chip for.
 */
struct line_settings {
    uint32_t baud;         // 0 for B0, which lowers DTR and RTS, so a chip is never asked for it
    uint8_t  data_bits;    // 5 to 8
    parity   parity_mode;
    uint8_t  stop_bits;    // 1 or 2
    bool     uses_rts_cts; // the chip sends only while CTS is raised
};

/**
 * What a serial port is and where it is attached. Its name is made of these.
 */
struct identity {
    const char* chip;          // the chip's short name in lowercase letters and digits, such as "cp2102"
    const char* serial_number; // "" when the device names none
    const char* location;      // digits and dots in its bus's numbering, such as "1.5.3", or "" when unknown
    int8_t      channel;       // the port's number on its device, 0 to 25, or -1 on a device with one port
};

} // namespace serial

#endif // STELLUX_DRIVERS_SERIAL_SERIAL_H
