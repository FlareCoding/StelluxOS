#ifndef STELLUX_IO_SERIAL_H
#define STELLUX_IO_SERIAL_H

#include "common/types.h"

namespace serial {

// Error codes
constexpr int32_t OK = 0;
constexpr int32_t ERR_NO_DATA = -1;
constexpr int32_t ERR_NO_DEVICE = -2;

/**
 * @brief Initialize the serial port hardware.
 * @return OK on success, negative error code on failure.
 */
int32_t init();

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
 * @brief Read a single byte from the serial port (non-blocking).
 * @return 0-255 on success (the byte read), or ERR_NO_DATA if no data available.
 */
int32_t read_char();

} // namespace serial

#endif // STELLUX_IO_SERIAL_H
