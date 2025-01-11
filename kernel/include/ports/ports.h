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

/**
 * @brief Marks an I/O port as privileged.
 * @param port The I/O port to mark as privileged.
 * 
 * Configures the specified I/O port to require elevated privileges for access. This is typically 
 * used to restrict access to sensitive hardware or low-level system resources.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mark_port_privileged(uint16_t port);

/**
 * @brief Marks an I/O port as unprivileged.
 * @param port The I/O port to mark as unprivileged.
 * 
 * Configures the specified I/O port to allow access without elevated privileges. This is typically 
 * used for ports that interact with user-space applications or less sensitive hardware.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mark_port_unprivileged(uint16_t port);

/**
 * @brief Marks an I/O port as privileged for a specific CPU.
 * 
 * Configures the specified I/O port to require elevated privileges for access on the provided CPU. This is typically 
 * used to restrict access to sensitive hardware or low-level system resources in systems with CPU-specific I/O control.
 * 
 * @param port The I/O port to mark as privileged.
 * @param cpu The ID of the CPU whose I/O bitmap should be modified.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mark_port_privileged(uint16_t port, uint8_t cpu);

/**
 * @brief Marks an I/O port as unprivileged for a specific CPU.
 * 
 * Configures the specified I/O port to allow access without elevated privileges on the provided CPU. This is typically 
 * used for ports that interact with user-space applications or less sensitive hardware in systems with CPU-specific 
 * I/O control.
 * 
 * @param port The I/O port to mark as unprivileged.
 * @param cpu The ID of the CPU whose I/O bitmap should be modified.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mark_port_unprivileged(uint16_t port, uint8_t cpu);

#endif
