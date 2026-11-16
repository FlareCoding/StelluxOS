#ifndef STELLUX_ARCH_UART_H
#define STELLUX_ARCH_UART_H

#include "common/types.h"

namespace serial::uart { class device; }

namespace arch {

/**
 * @brief The UART built into the platform, made with its board's registers,
 * clock and interrupt. It lives as long as the kernel and starts uninitialized.
 */
serial::uart::device* builtin_uart();

} // namespace arch

#endif // STELLUX_ARCH_UART_H
