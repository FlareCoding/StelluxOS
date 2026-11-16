#ifndef STELLUX_DRIVERS_SERIAL_UART_UART_H
#define STELLUX_DRIVERS_SERIAL_UART_UART_H

#include "common/types.h"

namespace serial::uart {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_NO_DEVICE   = -1;
constexpr int32_t ERR_UNSUPPORTED = -2;

// A UART samples each bit 16 times, so its reference clock divides by 16 times the speed
constexpr uint32_t SAMPLES_PER_BIT = 16;

// A UART samples each bit in its middle, so a speed more than 3 percent off misreads a 10-bit character
constexpr uint32_t BAUD_TOLERANCE_PERCENT = 3;

// A UART sends 5 to 8 data bits per character
constexpr uint8_t DATA_BITS_MIN = 5;
constexpr uint8_t DATA_BITS_MAX = 8;

enum class parity : uint8_t {
    none,
    odd,
    even,
};

/**
 * The speed and character format a UART runs with.
 */
struct settings {
    uint32_t baud;
    uint8_t  data_bits;   // DATA_BITS_MIN to DATA_BITS_MAX
    parity   parity_mode;
    uint8_t  stop_bits;   // 1 or 2
};

using receive_callback = void (*)(void* context, uint8_t byte);

/**
 * A `device` is one UART, a chip that sends and receives bytes one bit at a
 * time over a serial line. Each chip driver derives from this class, one
 * object per UART, and each UART has one owner chosen at boot.
 *
 * The owner calls `init` first, then turns on the receive interrupt and
 * writes. The UART hands each received byte to the owner's callback, from
 * interrupt context. `write` must poll the chip, take no lock and allocate
 * no memory, so it works in early boot and while the kernel panics.
 */
class device {
public:
    /**
     * @brief Programs the chip from cold with `line`, turning its receive
     * interrupt off and emptying its FIFOs.
     * @return OK, ERR_UNSUPPORTED for a format the chip lacks or a speed it
     *         cannot make within BAUD_TOLERANCE_PERCENT, or ERR_NO_DEVICE
     *         when the chip cannot be reached.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual int32_t init(const settings& line) = 0;

    /**
     * @brief Moves register access from the early boot mapping to the kernel's.
     * Must be called once after `mm::init()` for a UART initialized before it,
     * since other CPUs run without the early mapping.
     * @return OK, or ERR_NO_DEVICE when the registers cannot be mapped.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual int32_t remap_registers() = 0;

    /**
     * @brief Routes the receive interrupt and turns it on, so that each
     * received byte reaches `on_receive` with `context`. Must be called after
     * `irq::init()`.
     * @return OK, or ERR_UNSUPPORTED when the interrupt cannot be delivered.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual int32_t enable_receive_interrupt(receive_callback on_receive, void* context) = 0;

    /**
     * @brief Hands every byte in the receive FIFO to the callback. Called by
     * the receive interrupt's handler.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void handle_receive_interrupt() = 0;

    /**
     * @brief Sends `len` bytes, waiting for the chip to take each one.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void write(const char* data, size_t len) = 0;

protected:
    ~device() = default;
};

/**
 * @brief The divisor of a UART's reference clock that gives `baud`, in units of
 * `1 / 2^fraction_bits` for a chip whose divisor holds that many fraction bits,
 * rounded to the nearest unit. `baud` must not be 0.
 */
constexpr uint64_t baud_divisor(uint32_t clock_hz, uint32_t baud, uint32_t fraction_bits) {
    uint64_t scaled_clock = static_cast<uint64_t>(clock_hz) << fraction_bits;
    uint64_t clock_per_divisor = static_cast<uint64_t>(baud) * SAMPLES_PER_BIT;

    return (scaled_clock + clock_per_divisor / 2) / clock_per_divisor;
}

/**
 * @brief Whether a whole `divisor` of `clock_hz` makes `baud` within
 * BAUD_TOLERANCE_PERCENT.
 */
constexpr bool is_within_tolerance(uint32_t clock_hz, uint32_t baud, uint32_t divisor) {
    uint64_t needed_clock = static_cast<uint64_t>(baud) * SAMPLES_PER_BIT * divisor;
    uint64_t error = clock_hz > needed_clock ? clock_hz - needed_clock : needed_clock - clock_hz;

    return error * 100 <= needed_clock * BAUD_TOLERANCE_PERCENT;
}

} // namespace serial::uart

#endif // STELLUX_DRIVERS_SERIAL_UART_UART_H
