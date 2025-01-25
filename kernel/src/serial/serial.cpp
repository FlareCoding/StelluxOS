#include <serial/serial.h>

namespace serial {
uint16_t g_kernel_uart_port = SERIAL_PORT_BASE_COM1;
uint16_t g_kernel_gdb_stub_uart_port = SERIAL_PORT_BASE_COM2;

void init_port(uint16_t port, uint16_t baud_rate_divisor) {
    // Disable all interrupts
    outb(SERIAL_INTERRUPT_ENABLE_PORT(port), 0x00);

    // Configure the baud rate
    set_baud_rate(port, baud_rate_divisor);

    // Configure line control: 8 bits, no parity, 1 stop bit
    outb(SERIAL_LINE_COMMAND_PORT(port), SERIAL_LCR_8_BITS_NO_PARITY_ONE_STOP);

    // Enable FIFO, clear TX/RX queues, set interrupt trigger level to 14 bytes
    outb(SERIAL_FIFO_COMMAND_PORT(port),
         SERIAL_FCR_ENABLE_FIFO |
         SERIAL_FCR_CLEAR_RECEIVE_FIFO |
         SERIAL_FCR_CLEAR_TRANSMIT_FIFO |
         SERIAL_FCR_TRIGGER_14_BYTES);

    // Set RTS, DSR, and OUT2 to enable interrupts
    outb(SERIAL_MODEM_COMMAND_PORT(port),
         SERIAL_MCR_RTS_DSR | SERIAL_MCR_OUT2);

    // Enable "Received Data Available" interrupt
    outb(SERIAL_INTERRUPT_ENABLE_PORT(port), 0x01);
}

void set_baud_rate(uint16_t port, uint16_t divisor) {
    // Enable DLAB (Divisor Latch Access)
    outb(SERIAL_LINE_COMMAND_PORT(port), SERIAL_LCR_ENABLE_DLAB);

    // Set baud rate divisor
    outb(SERIAL_DATA_PORT(port), divisor & 0xFF); // Low byte
    outb(SERIAL_INTERRUPT_ENABLE_PORT(port), (divisor >> 8) & 0xFF); // High byte

    // Clear DLAB after setting the divisor
    outb(SERIAL_LINE_COMMAND_PORT(port), SERIAL_LCR_8_BITS_NO_PARITY_ONE_STOP);
}

bool is_transmit_queue_empty(uint16_t port) {
    return inb(SERIAL_LINE_STATUS_PORT(port)) & SERIAL_LSR_TRANSMIT_EMPTY;
}

bool is_data_available(uint16_t port) {
    return inb(SERIAL_LINE_STATUS_PORT(port)) & SERIAL_LSR_DATA_READY;
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
        if (*str == '\n') {
            // Treat "\n" as the CRLF ("\n\r") combo
            write(port, '\r');
        }
        ++str;
    }
}

char read(uint16_t port) {
    // Wait until data is available
    while (!is_data_available(port));
    
    // Read and return the character from the data port
    return inb(SERIAL_DATA_PORT(port));
}

void set_kernel_uart_port(uint16_t port) {
    g_kernel_uart_port = port;
}

__PRIVILEGED_CODE
void mark_serial_port_privileged(uint16_t serial_port) {
    // Mark every I/O port pertaining to the serial port base
    for (uint16_t i = serial_port; i <= serial_port + 6; i++) {
        mark_port_privileged(i);
    }
}

__PRIVILEGED_CODE
void mark_serial_port_unprivileged(uint16_t serial_port) {
    // Mark every I/O port pertaining to the serial port base
    for (uint16_t i = serial_port; i <= serial_port + 6; i++) {
        mark_port_unprivileged(i);
    }
}

__PRIVILEGED_CODE
void mark_serial_port_privileged(uint16_t serial_port, uint8_t cpu) {
    // Mark every I/O port pertaining to the serial port base
    for (uint16_t i = serial_port; i <= serial_port + 6; i++) {
        mark_port_privileged(i, cpu);
    }
}

__PRIVILEGED_CODE
void mark_serial_port_unprivileged(uint16_t serial_port, uint8_t cpu) {
    // Mark every I/O port pertaining to the serial port base
    for (uint16_t i = serial_port; i <= serial_port + 6; i++) {
        mark_port_unprivileged(i, cpu);
    }
}
} // namespace serial
