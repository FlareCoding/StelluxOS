#ifndef PORTS_H
#define PORTS_H
#include <types.h>

/**
 * @brief Writes an 8-bit value to the specified I/O port.
 * 
 * This function sends a single byte to the given I/O port, allowing for communication
 * with hardware devices mapped to that port. It is commonly used for sending control
 * commands or data to peripherals.
 * 
 * @param port The I/O port address to write to.
 * @param value The 8-bit value to be written to the port.
 */
void outb(uint16_t port, uint8_t value);

/**
 * @brief Reads an 8-bit value from the specified I/O port.
 * 
 * This function retrieves a single byte from the given I/O port, enabling the program
 * to receive data or status information from hardware devices mapped to that port.
 * 
 * @param port The I/O port address to read from.
 * @return uint8_t The 8-bit value read from the port.
 */
uint8_t inb(uint16_t port);

/**
 * @brief Writes a 16-bit value to the specified I/O port.
 * 
 * This function sends a two-byte word to the given I/O port, facilitating communication
 * with hardware devices that require 16-bit data transfers. It is useful for sending
 * larger control commands or data blocks.
 * 
 * @param port The I/O port address to write to.
 * @param val The 16-bit value to be written to the port.
 */
void outw(uint16_t port, uint16_t val);

/**
 * @brief Reads a 16-bit value from the specified I/O port.
 * 
 * This function retrieves a two-byte word from the given I/O port, allowing the program
 * to receive larger chunks of data or more detailed status information from hardware
 * devices mapped to that port.
 * 
 * @param port The I/O port address to read from.
 * @return uint16_t The 16-bit value read from the port.
 */
uint16_t inw(uint16_t port);

/**
 * @brief Writes a 32-bit value to the specified I/O port.
 * 
 * This function sends a four-byte double word to the given I/O port, enabling communication
 * with hardware devices that utilize 32-bit data transfers. It is suitable for sending
 * extensive control commands or large data blocks.
 * 
 * @param port The I/O port address to write to.
 * @param val The 32-bit value to be written to the port.
 */
void outl(uint16_t port, uint32_t val);

/**
 * @brief Reads a 32-bit value from the specified I/O port.
 * 
 * This function retrieves a four-byte double word from the given I/O port, allowing the
 * program to receive substantial amounts of data or comprehensive status information from
 * hardware devices mapped to that port.
 * 
 * @param port The I/O port address to read from.
 * @return uint32_t The 32-bit value read from the port.
 */
uint32_t inl(uint16_t port);

#endif