#ifndef STELLUX_SERIAL_SERIAL_H
#define STELLUX_SERIAL_SERIAL_H

#include "common/types.h"

namespace serial {

// Error codes
constexpr int32_t OK = 0;
constexpr int32_t ERR_NO_DEVICE = -2;

using rx_callback_t = void (*)(char c);

/**
 * @brief Initialize the serial port hardware.
 * @return OK on success, negative error code on failure.
 */
int32_t init();

/**
 * @brief Switch serial from early boot mapping to kernel virtual address.
 * On AArch64: remaps UART via vmm::map_device (TTBR1) so serial works
 * after TTBR0 is cleared on APs. Must be called after mm::init().
 * On x86_64: no-op (serial uses port I/O).
 * @return OK on success, negative error code on failure.
 */
int32_t remap();

/**
 * @brief Write a single byte to the serial port.
 * Blocks until the transmit buffer is ready.
 */
void write_char(char c);

/**
 * @brief Write a buffer of bytes to the serial port.
 * Blocks for each byte until transmission completes.
 */
void write(const char* data, size_t len);

/**
 * @brief Redirect serial output to a different I/O port.
 * Initializes the new port at 115200 baud and, when a chip answers
 * there, redirects all subsequent I/O to it. Does nothing on an
 * architecture without I/O ports.
 * @param port I/O port base address of the new UART (e.g. from a PCI BAR).
 */
void set_port(uint16_t port);

/**
 * @brief Register a callback invoked for each received character.
 * Called from interrupt context; the callback must be __PRIVILEGED_CODE.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_rx_callback(rx_callback_t cb);

/**
 * @brief Enable the hardware RX interrupt and route it through the
 * interrupt controller. Must be called after irq::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t enable_rx_interrupt();

/**
 * @brief Serial RX interrupt service routine. Drains the hardware FIFO
 * and invokes the registered callback for each byte.
 * Called from the arch trap handler.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void on_rx_irq();

} // namespace serial

#endif // STELLUX_SERIAL_SERIAL_H
