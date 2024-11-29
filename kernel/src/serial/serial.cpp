#include <serial/serial.h>

namespace serial {
void init_port(uint16_t port) {
    outb(SERIAL_LINE_COMMAND_PORT(port), SERIAL_LCR_DISABLE_ALL_INTERRUPTS);
    outb(SERIAL_LINE_COMMAND_PORT(port), SERIAL_LINE_ENABLE_DLAB);
    outb(SERIAL_DATA_PORT(port), SERIAL_BAUD_RATE_DIVISOR_LOW); 
    outb(SERIAL_DATA_PORT(port) + 1, SERIAL_BAUD_RATE_DIVISOR_HIGH);
    outb(SERIAL_LINE_COMMAND_PORT(port), SERIAL_LCR_EIGHT_BITS_NO_PARITY_ONE_STOP);
    outb(SERIAL_FIFO_COMMAND_PORT(port), SERIAL_FIFO_CTRL_ENABLE_CLEAR_14BYTES);
    outb(SERIAL_MODEM_COMMAND_PORT(port), SERIAL_MCR_ENABLE_IRQ_RTS_DSR);

    // Enable "Received Data Available" interrupt in the Interrupt Enable Register (IER)
    outb(port + 1, SERIAL_INTERRUPT_DATA_AVAILABLE);
}

bool is_transmit_queue_empty(uint16_t port) {
    return inb(SERIAL_LINE_STATUS_PORT(port)) & SERIAL_LINE_STATUS_THR_EMPTY;
}

bool is_data_available(uint16_t port) {
    return inb(SERIAL_LINE_STATUS_PORT(port)) & SERIAL_LINE_STATUS_DATA_READY;
}

void write(uint16_t port, char chr) {
    // Wait for the transmit queue to be empty
    while (!is_transmit_queue_empty(port));

    // Write the byte to the serial port
    outb(port, chr);
}

void write(uint16_t port, const char* str) {
    while (*str != '\0') {
        write(port, *str);
        ++str;
    }
}

char read(uint16_t port) {
    // Wait until data is available
    while (!is_data_available(port));
    
    // Read and return the character from the data port
    return inb(SERIAL_DATA_PORT(port));
}
} // namespace serial
