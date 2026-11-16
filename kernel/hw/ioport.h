#ifndef STELLUX_HW_IOPORT_H
#define STELLUX_HW_IOPORT_H

#include "common/types.h"

namespace ioport {

/**
 * @brief Whether this architecture reaches devices through I/O ports.
 * x86_64: yes, through its port instructions.
 * AArch64: no, so reads return all ones and writes do nothing, as an I/O port
 * with no device behind it does on a PC.
 */
bool available();

/**
 * @brief Read an 8-bit value from I/O port `port`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint8_t read8(uint16_t port);

/**
 * @brief Write an 8-bit value to I/O port `port`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void write8(uint16_t port, uint8_t value);

} // namespace ioport

#endif // STELLUX_HW_IOPORT_H
