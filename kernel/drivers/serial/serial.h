#ifndef STELLUX_DRIVERS_SERIAL_SERIAL_H
#define STELLUX_DRIVERS_SERIAL_SERIAL_H

#include "common/types.h"

namespace serial {

// serial::uart gives -1 to ERR_NO_DEVICE, so each name keeps one value in both namespaces
constexpr int32_t OK              = 0;
constexpr int32_t ERR_UNSUPPORTED = -2;
constexpr int32_t ERR_NO_MEMORY   = -3;

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

/**
 * A serial port hands the bytes a UART receives to programs through a file
 * named for the board, such as /dev/serial/ft232bm-0001. A driver registers a
 * port for each UART, passes it what the UART receives, and detaches it when
 * the device leaves.
 */
class port;

/**
 * @brief Creates /dev/serial, where ports appear.
 * @return OK, or ERR_NO_MEMORY.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Adds a port for the UART `id` describes as /dev/serial/<name>, under
 * the first name the naming rules leave free, and keeps a copy of `id`.
 * Allocates and logs, so it must run on the driver's own task.
 * @return The port, or nullptr when memory runs out, a string of `id` is
 *         longer than a name, or no free name fits.
 */
port* register_port(const identity& id);

/**
 * @brief Queues bytes the chip received for reads, dropping them while no file
 * has the port open and once the queue is full. Never blocks.
 */
void receive(port* p, const uint8_t* data, size_t len);

/**
 * @brief Records that the chip delivers no more bytes, so reads return what is
 * queued, then fail. Never blocks.
 */
void receive_failed(port* p);

/**
 * @brief Takes the port's file out of /dev/serial and frees its name, once its
 * device has left. Reads return what is queued, then end of file. A driver
 * must call it exactly once. Never blocks, so a driver's `disconnect()` may
 * call it.
 */
void detach(port* p);

/**
 * @brief Drops the registry's reference to a detached port. The driver must
 * call it only after its last call into the port has returned, and the port
 * stays allocated until its last open file closes.
 */
void unregister_port(port* p);

} // namespace serial

#endif // STELLUX_DRIVERS_SERIAL_SERIAL_H
