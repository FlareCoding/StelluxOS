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
    void init_port(uint16_t port);
    
    bool is_transmit_queue_empty(uint16_t port);

    bool is_data_available(uint16_t port);

    void write(uint16_t port, char chr);

    void write(uint16_t port, const char* str);

    char read(uint16_t port);
};

#endif