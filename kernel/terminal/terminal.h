#ifndef STELLUX_TERMINAL_TERMINAL_H
#define STELLUX_TERMINAL_TERMINAL_H

#include "common/types.h"

struct ring_buffer;
namespace resource { struct resource_ops; }

namespace terminal {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

// Stellux-internal line-discipline mode-switch ioctls.
// Encoded as _IO('s', N) so they do not collide with Linux's tty ioctl
constexpr uint32_t STLX_TCSETS_RAW    = 0x7301;
constexpr uint32_t STLX_TCSETS_COOKED = 0x7302;

/**
 * @brief Claims the UART built into the platform for the kernel log and
 * /dev/console. Must be called after boot_services::init() and before anything
 * logs, since log lines written before the console holds a UART are dropped.
 * @return OK, or ERR when the UART cannot be reached.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t claim_builtin_uart();

/**
 * @brief Moves the console UART's register access from the early boot mapping
 * to the kernel's. Must be called once after mm::init().
 * @return OK, or ERR when the registers cannot be mapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t remap_uart();

/**
 * @brief Moves the console to the UART of a PCI serial card when one is
 * present and answers, since many PCs, desktops above all, lack a usable COM1.
 * Must be called once after pci::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void claim_pci_uart();

/**
 * @brief Writes to the console's UART. It polls the chip, takes no lock and
 * allocates no memory, so it works in early boot and while the kernel panics.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void console_write(const char* data, size_t len);

/**
 * @brief Initialize the global console terminal. Creates the input ring
 * buffer, registers as the serial RX callback, enables serial RX
 * interrupts, and creates /dev/console in devfs.
 * Must be called after claim_builtin_uart(), irq::init() and fs::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Feed a received byte into the terminal's line discipline.
 * Matches serial::uart::receive_callback signature. Called from ISR context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void input_char(void* context, uint8_t byte);

/**
 * @brief Get the console terminal's input ring buffer.
 * Callers must be elevated to access the returned pointer.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ring_buffer* console_input_rb();

/**
 * @brief Read the console's input through its line discipline, so an end
 * of file typed in cooked mode reads as 0.
 * @return As ld_read.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t console_read(uint8_t* buf, size_t len, bool nonblock);

/**
 * @brief Switch the console terminal between raw and cooked mode.
 * Elevates internally for the spinlock critical section.
 * @param cmd STLX_TCSETS_RAW or STLX_TCSETS_COOKED.
 * @return OK on success, ERR on invalid cmd.
 */
int32_t set_mode(uint32_t cmd);

/**
 * @brief Console terminal ioctl handling: the termios, window size and
 * foreground process group requests in tty.h, plus the mode-switch
 * commands set_mode accepts. The window size reads 0 by 0, meaning
 * unknown, until a program sets it. TCSETSF applies settings as TCSETS
 * does and keeps input no program has read.
 * @return OK on success, fs::ERR_INVAL for an argument the kernel cannot
 * read or write or a negative or missing foreground group, fs::ERR_NOSYS
 * for any other cmd.
 */
int32_t console_ioctl(uint32_t cmd, uint64_t arg);

/**
 * @brief Get the terminal resource ops table for creating resource_objects.
 */
const resource::resource_ops* get_terminal_ops();

} // namespace terminal

#endif // STELLUX_TERMINAL_TERMINAL_H
