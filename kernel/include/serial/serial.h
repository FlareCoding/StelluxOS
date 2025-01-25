#ifndef SERIAL_H
#define SERIAL_H
#include <ports/ports.h>
#include <string.h>

// Base UART I/O ports
#define SERIAL_PORT_BASE_COM1 0x3F8
#define SERIAL_PORT_BASE_COM2 0x2F8
#define SERIAL_PORT_BASE_COM3 0x3EF
#define SERIAL_PORT_BASE_COM4 0x2EF

// UART I/O Register Offsets
#define SERIAL_DATA_PORT(base)               (base + 0)
#define SERIAL_INTERRUPT_ENABLE_PORT(base)   (base + 1)
#define SERIAL_FIFO_COMMAND_PORT(base)       (base + 2)
#define SERIAL_LINE_COMMAND_PORT(base)       (base + 3)
#define SERIAL_MODEM_COMMAND_PORT(base)      (base + 4)
#define SERIAL_LINE_STATUS_PORT(base)        (base + 5)

// UART Line Control Register (LCR) Flags
#define SERIAL_LCR_ENABLE_DLAB               0x80 // Enable Divisor Latch Access
#define SERIAL_LCR_8_BITS_NO_PARITY_ONE_STOP 0x03 // 8 bits, no parity, 1 stop bit

// UART FIFO Control Register (FCR) Flags
#define SERIAL_FCR_ENABLE_FIFO               0x01 // Enable FIFO
#define SERIAL_FCR_CLEAR_RECEIVE_FIFO        0x02 // Clear Receive FIFO
#define SERIAL_FCR_CLEAR_TRANSMIT_FIFO       0x04 // Clear Transmit FIFO
#define SERIAL_FCR_TRIGGER_14_BYTES          0xC0 // Set trigger level to 14 bytes

// UART Modem Control Register (MCR) Flags
#define SERIAL_MCR_RTS_DSR                   0x03 // Ready to Send (RTS), Data Set Ready (DSR)
#define SERIAL_MCR_OUT2                      0x08 // OUT2 bit, required to enable UART interrupts

// UART Line Status Register (LSR) Flags
#define SERIAL_LSR_TRANSMIT_EMPTY            0x20 // Transmitter Holding Register Empty
#define SERIAL_LSR_DATA_READY                0x01 // Data Ready

// Common Baud Rates Divisors (assuming 1.8432 MHz clock)
#define SERIAL_BAUD_DIVISOR_115200           0x01 // 115200 baud
#define SERIAL_BAUD_DIVISOR_57600            0x02 // 57600 baud
#define SERIAL_BAUD_DIVISOR_38400            0x03 // 38400 baud
#define SERIAL_BAUD_DIVISOR_19200            0x06 // 19200 baud
#define SERIAL_BAUD_DIVISOR_9600             0x0C // 9600 baud
#define SERIAL_BAUD_DIVISOR_4800             0x18 // 4800 baud
#define SERIAL_BAUD_DIVISOR_2400             0x30 // 2400 baud
#define SERIAL_BAUD_DIVISOR_1200             0x60 // 1200 baud

namespace serial {
// I/O address of the UART port to which kernel's serial prints will get directed
extern uint16_t g_kernel_uart_port;

// I/O address of the UART port to be used for the GDB stub
extern uint16_t g_kernel_gdb_stub_uart_port;

/**
 * @brief Initializes the specified serial port with default settings.
 * 
 * This function configures the serial port by setting up the baud rate, 
 * line control, FIFO settings, and modem control registers. It prepares 
 * the port for communication by enabling necessary interrupts and setting 
 * data frame formats.
 * 
 * @param port The I/O port address of the serial port to initialize.
 * @param baud_rate_divisor  The divisor value for the desired baud rate.
 */
void init_port(uint16_t port, uint16_t baud_rate_divisor = SERIAL_BAUD_DIVISOR_9600);

/**
 * @brief Sets the baud rate for the specified serial port.
 *
 * This function configures the Divisor Latch registers (DLAB) to set the baud rate.
 *
 * @param port The I/O port address of the serial port to configure.
 * @param divisor The divisor value for the desired baud rate.
 */
void set_baud_rate(uint16_t port, uint16_t divisor);

/**
 * @brief Checks if the transmit queue is empty for the specified serial port.
 * 
 * This function reads the Line Status Register (LSR) to determine if the 
 * Transmit Holding Register (THR) is empty, indicating that all queued 
 * data has been transmitted and the port is ready to send more data.
 * 
 * @param port The I/O port address of the serial port to check.
 * @return true If the transmit queue is empty and the port is ready to send data.
 * @return false If there are still bytes pending in the transmit queue.
 */
bool is_transmit_queue_empty(uint16_t port);

/**
 * @brief Determines if there is incoming data available to read from the serial port.
 * 
 * This function checks the Line Status Register (LSR) to verify if data has 
 * been received and is ready to be read from the receive buffer. It is useful 
 * for non-blocking read operations to ensure data is available before attempting to read.
 * 
 * @param port The I/O port address of the serial port to monitor.
 * @return true If there is data available in the receive buffer.
 * @return false If no data is currently available to read.
 */
bool is_data_available(uint16_t port);

/**
 * @brief Sends a single character through the specified serial port.
 * 
 * This function writes a single character to the Transmit Holding Register (THR) 
 * of the serial port, initiating the transmission of the character over the serial line.
 * It ensures that the transmit queue is ready to accept new data before writing.
 * 
 * @param port The I/O port address of the serial port to use for sending the character.
 * @param chr The character to be transmitted.
 */
void write(uint16_t port, char chr);

/**
 * @brief Sends a null-terminated string through the specified serial port.
 * 
 * This function iterates over each character in the provided string and transmits 
 * them sequentially using the serial port. It ensures that the transmit queue is 
 * ready for each character before sending, allowing for reliable string transmission.
 * 
 * @param port The I/O port address of the serial port to use for sending the string.
 * @param str The null-terminated string to be transmitted.
 */
void write(uint16_t port, const char* str);

/**
 * @brief Reads a single character from the specified serial port.
 * 
 * This function retrieves a character from the Receive Buffer Register (RBR) of 
 * the serial port. It should be called only when data is confirmed to be available 
 * using `is_data_available` to avoid reading invalid or empty data.
 * 
 * @param port The I/O port address of the serial port to read from.
 * @return char The character read from the serial port.
 */
char read(uint16_t port);

/**
 * @brief Sets the port to which kernel directs all serial `printf` calls.
 * 
 * @param port The I/O address of the port to which direct all kernel serial prints. 
 */
void set_kernel_uart_port(uint16_t port);

/**
 * @brief Prints a formatted string to the COM1 serial port.
 *
 * @tparam Args Variadic template arguments corresponding to format specifiers.
 * @param format The format string.
 * @param args The arguments to format.
 * @return int The number of characters written, excluding the null terminator.
 */
template <typename... Args>
int printf(const char* format, Args... args) {
    // Define a buffer with a fixed size. Adjust as necessary.
    constexpr size_t BUFFER_SIZE = 256;
    char buffer[BUFFER_SIZE] = { 0 };
    
    // Format the string using the custom sprintf
    int len = sprintf(buffer, BUFFER_SIZE, format, args...);
    
    // Send the formatted string over the target UART port
    serial::write(g_kernel_uart_port, buffer);
    
    return len;
}

/**
 * @brief Marks a serial port as privileged.
 * 
 * Configures the specified serial port to require elevated privileges for access. This is 
 * typically used to restrict access to critical or sensitive serial ports.
 * 
 * @param serial_port The starting I/O port address of the serial port to mark as privileged.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mark_serial_port_privileged(uint16_t serial_port);

/**
 * @brief Marks a serial port as unprivileged.
 * 
 * Configures the specified serial port to allow access without elevated privileges. This is 
 * typically used for serial ports that interact with user-space applications or general-purpose hardware.
 * 
 * @param serial_port The starting I/O port address of the serial port to mark as unprivileged.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mark_serial_port_unprivileged(uint16_t serial_port);

/**
 * @brief Marks a serial port as privileged for a specific CPU.
 * 
 * Configures the specified serial port to require elevated privileges for access on the provided CPU. This is 
 * typically used to restrict access to critical or sensitive serial ports in systems with CPU-specific I/O control.
 * 
 * @param serial_port The starting I/O port address of the serial port to mark as privileged.
 * @param cpu The ID of the CPU whose I/O bitmap should be modified.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mark_serial_port_privileged(uint16_t serial_port, uint8_t cpu);

/**
 * @brief Marks a serial port as unprivileged for a specific CPU.
 * 
 * Configures the specified serial port to allow access without elevated privileges on the provided CPU. This is 
 * typically used for serial ports that interact with user-space applications or general-purpose hardware in 
 * systems with CPU-specific I/O control.
 * 
 * @param serial_port The starting I/O port address of the serial port to mark as unprivileged.
 * @param cpu The ID of the CPU whose I/O bitmap should be modified.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mark_serial_port_unprivileged(uint16_t serial_port, uint8_t cpu);
} // namespace serial

#endif