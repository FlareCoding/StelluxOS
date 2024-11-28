#ifndef SERIAL_H
#define SERIAL_H
#include <ports/ports.h>

// Base I/O port for the first serial port
#define SERIAL_PORT_BASE_COM1 0x3F8
#define SERIAL_PORT_BASE_COM2 0x2F8
#define SERIAL_PORT_BASE_COM3 0x3EF
#define SERIAL_PORT_BASE_COM4 0x2EF

// Offsets for specific serial port registers
#define SERIAL_DATA_PORT(base)      (base)
#define SERIAL_FIFO_COMMAND_PORT(base) (base + 2)
#define SERIAL_LINE_COMMAND_PORT(base) (base + 3)
#define SERIAL_MODEM_COMMAND_PORT(base) (base + 4)
#define SERIAL_LINE_STATUS_PORT(base) (base + 5)

// Line control register (LCR) values
#define SERIAL_LINE_ENABLE_DLAB 0x80

// Line status register (LSR) values
#define SERIAL_LINE_STATUS_DATA_READY 0x01 // Data ready in receive buffer

// Line status register (LSR) values
#define SERIAL_LINE_STATUS_THR_EMPTY 0x20  // Transmit-hold-register empty

// Disable all interrupts on the line control register
#define SERIAL_LCR_DISABLE_ALL_INTERRUPTS 0x00

// Baud rate divisor value (low byte and high byte)
// for a baud rate of 38400
#define SERIAL_BAUD_RATE_DIVISOR_LOW 0x03
#define SERIAL_BAUD_RATE_DIVISOR_HIGH 0x00

// Data frame format: 8 data bits, no parity, one stop bit
#define SERIAL_LCR_EIGHT_BITS_NO_PARITY_ONE_STOP 0x03

// Enable FIFO, clear both receiver and transmitter FIFOs, 
// and set the interrupt level at 14 bytes
#define SERIAL_FIFO_CTRL_ENABLE_CLEAR_14BYTES 0xC7

// Enable IRQs, and set RTS and DSR
#define SERIAL_MCR_ENABLE_IRQ_RTS_DSR 0x0B

// Interrupt when data is received
#define SERIAL_INTERRUPT_DATA_AVAILABLE 0x01

namespace serial {
    /**
     * @brief Initializes the specified serial port with default settings.
     * 
     * This function configures the serial port by setting up the baud rate, 
     * line control, FIFO settings, and modem control registers. It prepares 
     * the port for communication by enabling necessary interrupts and setting 
     * data frame formats.
     * 
     * @param port The I/O port address of the serial port to initialize.
     */
    void init_port(uint16_t port);
    
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
};

#endif