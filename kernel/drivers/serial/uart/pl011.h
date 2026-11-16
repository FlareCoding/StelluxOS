#ifndef STELLUX_DRIVERS_SERIAL_UART_PL011_H
#define STELLUX_DRIVERS_SERIAL_UART_PL011_H

#include "drivers/serial/uart/uart.h"

namespace serial::uart {

// Prepares the interrupt controller for a PL011's interrupt before the PL011 unmasks it
using prepare_interrupt_fn = void (*)(uint32_t irq);

/**
 * An Arm PL011 reached through MMIO. It is made with its registers' physical
 * address, its reference clock, its interrupt number as `irq::` takes it, and
 * the platform function that prepares that interrupt, or nullptr when it needs
 * nothing. `init` maps the registers through the early boot tables, so a PL011
 * is initialized before `mm::init()` and remapped after it.
 */
class pl011 final : public device {
public:
    constexpr pl011(uintptr_t phys, uint32_t clock_hz, uint32_t irq, prepare_interrupt_fn prepare_receive_interrupt)
        : m_phys(phys)
        , m_base(0)
        , m_clock_hz(clock_hz)
        , m_irq(irq)
        , m_prepare_receive_interrupt(prepare_receive_interrupt)
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
     * @brief The divisor that gives `baud` from `clock_hz`, in 64ths.
     * @return The divisor, or ERR_UNSUPPORTED when `baud` needs one outside
     *         1 to 65535.
     */
    static int32_t divisor_for(uint32_t clock_hz, uint32_t baud);

    /**
     * @brief The line control value that selects `line`'s character format,
     * with the FIFOs on.
     * @return The value, or ERR_UNSUPPORTED for a format the chip lacks.
     */
    static int32_t line_control_for(const settings& line);

private:
    uintptr_t            m_phys;
    uintptr_t            m_base; // Virtual address of the registers, 0 until init maps them
    uint32_t             m_clock_hz;
    uint32_t             m_irq;
    prepare_interrupt_fn m_prepare_receive_interrupt;
    receive_callback     m_on_receive;
    void*                m_receive_context;
};

} // namespace serial::uart

#endif // STELLUX_DRIVERS_SERIAL_UART_PL011_H
