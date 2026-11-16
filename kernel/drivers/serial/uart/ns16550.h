#ifndef STELLUX_DRIVERS_SERIAL_UART_NS16550_H
#define STELLUX_DRIVERS_SERIAL_UART_NS16550_H

#include "drivers/serial/uart/uart.h"

namespace serial::uart {

// The reference clock most 16550s run from, 16 times 115200, which divides into the standard speeds
constexpr uint32_t NS16550_STANDARD_CLOCK_HZ = 1843200;

// Prepares the interrupt controller to deliver a 16550's receive interrupt, returning OK or ERR_UNSUPPORTED
using route_interrupt_fn = int32_t (*)();

/**
 * A 16550 reached through I/O ports, such as a PC's COM1 or a PCI serial card.
 * It is made with its first port, its reference clock and the platform function
 * that routes its receive interrupt, or nullptr when the platform cannot route it.
 * `init` reads the line status back to check that a chip answers at the port.
 */
class ns16550 final : public device {
public:
    constexpr ns16550(uint16_t port, uint32_t clock_hz, route_interrupt_fn route_receive_interrupt)
        : m_port(port)
        , m_clock_hz(clock_hz)
        , m_route_receive_interrupt(route_receive_interrupt)
        , m_on_receive(nullptr)
        , m_receive_context(nullptr) {}

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE int32_t init(const settings& line) override;
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE int32_t remap_registers() override;
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE int32_t enable_receive_interrupt(receive_callback on_receive, void* context) override;
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void handle_receive_interrupt() override;
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void write(const char* data, size_t len) override;

    /**
     * @brief The divisor latch value that gives `baud` from `clock_hz`.
     * @return The divisor, or ERR_UNSUPPORTED when no divisor makes `baud`
     *         within BAUD_TOLERANCE_PERCENT.
     */
    static int32_t divisor_for(uint32_t clock_hz, uint32_t baud);

    /**
     * @brief The line control value that selects `line`'s character format.
     * @return The value, or ERR_UNSUPPORTED for a format the chip lacks, such as
     *         2 stop bits with 5 data bits, which it sends as one and a half.
     */
    static int32_t line_control_for(const settings& line);

private:
    uint16_t           m_port;
    uint32_t           m_clock_hz;
    route_interrupt_fn m_route_receive_interrupt;
    receive_callback   m_on_receive;
    void*              m_receive_context;
};

} // namespace serial::uart

#endif // STELLUX_DRIVERS_SERIAL_UART_NS16550_H
