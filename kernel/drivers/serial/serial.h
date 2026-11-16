#ifndef STELLUX_DRIVERS_SERIAL_SERIAL_H
#define STELLUX_DRIVERS_SERIAL_SERIAL_H

#include "common/types.h"

namespace serial {

// serial::uart gives -1 to ERR_NO_DEVICE, so each name keeps one value in both namespaces
constexpr int32_t OK              = 0;
constexpr int32_t ERR_UNSUPPORTED = -2;
constexpr int32_t ERR_NO_MEMORY   = -3;
constexpr int32_t ERR_IO          = -4;

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
 * What a serial port is and where it is attached. Its name is made of the
 * first four fields, and its line in /dev/sysinfo/serial of all of them.
 */
struct identity {
    const char* chip;              // the chip's short name in lowercase letters and digits, such as "cp2102"
    const char* serial_number;     // "" when the device names none
    const char* location;          // digits and dots in its bus's numbering, such as "1.5.3", or "" when unknown
    int8_t      channel;           // the port's number on its device, 0 to 25, or -1 on a device with one port
    const char* bus = "";          // the bus the device is on, such as "usb", or "" when unknown
    const char* driver = "";       // the chip driver's name, such as "cp210x"
    uint16_t    vid = 0;           // the USB vendor ID, or 0 for a port not on USB
    uint16_t    pid = 0;           // the USB product ID
    const char* manufacturer = ""; // "" when the device names none
    const char* product = "";      // "" when the device names none
};

/**
 * A serial port hands the bytes a UART receives to programs through a file
 * named for the board, such as /dev/serial/ft232bm-0001, and sends what they
 * write, one send at a time. A driver registers a port for each UART, passes
 * it what the UART receives, and detaches it when the device leaves.
 */
class port;

// The most bytes one send carries, the most the xHCI driver moves in one transfer
constexpr size_t SEND_BYTES = 4096;

/**
 * A port's driver, which sends what programs write and applies the settings,
 * modem lines and breaks they ask for. The port calls `send` under its write
 * lock and the other calls one at a time under its control lock, so `send`
 * can run while another call does. `send` must return without waiting for the
 * chip, since non-blocking writes call it. The other calls may sleep until a
 * chip request ends at its reply, its deadline or the device's unplug.
 */
class port_driver {
public:
    virtual ~port_driver() = default;

    /**
     * @brief Sets the chip to `settings`, whose speed is never 0, at the
     * nearest rate the chip makes.
     * @return OK, ERR_UNSUPPORTED with nothing sent for a rate more than 3
     *         percent from every rate the chip makes or a field it lacks, or
     *         ERR_IO when a chip request fails.
     */
    virtual int32_t set_line(const line_settings& settings) = 0;

    /**
     * @brief Sets DTR and RTS to the TIOCM_DTR and TIOCM_RTS bits of `lines`,
     * both in one chip request.
     * @return OK, or ERR_IO when the chip request fails.
     */
    virtual int32_t set_modem_outputs(uint32_t lines) = 0;

    /**
     * @brief Gives the chip's CTS, DSR, RI and CD as TIOCM bits in `lines`.
     * @return OK, or ERR_IO when the chip request fails.
     */
    virtual int32_t get_modem_inputs(uint32_t* lines) = 0;

    /**
     * @brief Starts or ends a break, which holds the UART's transmit line low.
     * @return OK, or ERR_IO when the chip request fails.
     */
    virtual int32_t set_break(bool on) = 0;

    /**
     * @brief The bytes the chip still holds to send, or an estimate for a chip
     * that tells only whether it holds any, which the port waits out at the
     * line's speed before it asks again. At 0 the chip's queue is empty,
     * though a byte may still be leaving its UART, which the port allows for.
     * @return The count, or ERR_IO when the chip request fails.
     */
    virtual ssize_t tx_pending() = 0;

    /**
     * @brief Starts sending `length` bytes, at most SEND_BYTES, from `data`,
     * which the port leaves untouched until `send_ended` reports the send.
     * Called only with no send in flight.
     * @return OK, or ERR_IO when the send never started, after which no
     *         `send_ended` follows.
     */
    virtual int32_t send(const uint8_t* data, size_t length) = 0;

    /**
     * @brief Ends the send in flight and returns once `send_ended` has
     * reported it, or at once with none in flight.
     */
    virtual void cancel_send() = 0;
};

/**
 * @brief Whether a chip making `made` baud serves a port asked for `asked`,
 * which holds while they differ by at most 3 percent. A UART misreads a
 * 10-bit character once the two clocks differ by about 5 percent, so this
 * leaves room for the far end's error.
 */
bool is_rate_within_tolerance(uint32_t made, uint32_t asked);

/**
 * @brief Creates /dev/serial, where ports appear, and /dev/sysinfo/serial,
 * which lists them a line each and wakes its pollers as a port arrives or
 * leaves.
 * @return OK, or ERR_NO_MEMORY when /dev/serial cannot be created. A listing
 *         that cannot be added is only logged, since ports work without it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Adds a port for the UART `id` describes as /dev/serial/<name>, under
 * the first name the naming rules leave free, keeps a copy of `id`, calls
 * `driver` until `unregister_port`, and then lists the port. Allocates and
 * logs, so it must run on the driver's own task.
 * @return The port, or nullptr when memory runs out or no free name fits.
 */
port* register_port(port_driver* driver, const identity& id);

/**
 * @brief Queues bytes the chip received for reads, dropping them while no file
 * has the port open and once the queue is full. The listing counts every byte
 * as received and the bytes a full queue cannot hold as dropped. Never blocks.
 */
void receive(port* p, const uint8_t* data, size_t len);

/**
 * @brief Records that the chip delivers no more bytes, so reads return what is
 * queued, then fail. Never blocks.
 */
void receive_failed(port* p);

/**
 * @brief Reports the end of the port's send: the bytes the chip took, all of
 * them unless the send was cancelled, or ERR_IO when it failed, which fails
 * every later write and wait for a send. Never blocks.
 */
void send_ended(port* p, ssize_t taken_or_error);

/**
 * @brief Takes the port's line out of the listing and its file out of
 * /dev/serial, and frees its name, once its device has left. Reads return what
 * is queued, then end of file. A driver must call it exactly once. Never
 * blocks, so a driver's `disconnect()` may call it.
 */
void detach(port* p);

/**
 * @brief Drops the registry's reference to a detached port, and returns once
 * no call from the port into the driver is in progress or can follow. The
 * driver must call it only after its last call into the port has returned,
 * and the port stays allocated until its last open file closes.
 */
void unregister_port(port* p);

} // namespace serial

#endif // STELLUX_DRIVERS_SERIAL_SERIAL_H
