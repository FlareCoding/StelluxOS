#include "serial.h"

__PRIVILEGED_CODE
void initializeSerialPort(uint16_t base) {
    outByte(SERIAL_LINE_COMMAND_PORT(base), SERIAL_LCR_DISABLE_ALL_INTERRUPTS);
    outByte(SERIAL_LINE_COMMAND_PORT(base), SERIAL_LINE_ENABLE_DLAB);
    outByte(SERIAL_DATA_PORT(base), SERIAL_BAUD_RATE_DIVISOR_LOW); 
    outByte(SERIAL_DATA_PORT(base) + 1, SERIAL_BAUD_RATE_DIVISOR_HIGH);
    outByte(SERIAL_LINE_COMMAND_PORT(base), SERIAL_LCR_EIGHT_BITS_NO_PARITY_ONE_STOP);
    outByte(SERIAL_FIFO_COMMAND_PORT(base), SERIAL_FIFO_CTRL_ENABLE_CLEAR_14BYTES);
    outByte(SERIAL_MODEM_COMMAND_PORT(base), SERIAL_MCR_ENABLE_IRQ_RTS_DSR);

    // Enable "Received Data Available" interrupt in the Interrupt Enable Register (IER)
    outByte(base + 1, SERIAL_INTERRUPT_DATA_AVAILABLE);
}

__PRIVILEGED_CODE
bool isTransmitQueueEmpty(uint16_t base) {
    return inByte(SERIAL_LINE_STATUS_PORT(base)) & SERIAL_LINE_STATUS_THR_EMPTY;
}

__PRIVILEGED_CODE
bool isDataAvailable(uint16_t base) {
    return inByte(SERIAL_LINE_STATUS_PORT(base)) & SERIAL_LINE_STATUS_DATA_READY;
}

__PRIVILEGED_CODE
void writeToSerialPort(uint16_t base, char chr) {
    // Wait for the transmit queue to be empty
    while (!isTransmitQueueEmpty(base));

    // Write the byte to the serial port
    outByte(base, chr);
}

__PRIVILEGED_CODE
void writeToSerialPort(uint16_t base, const char* str) {
    while (*str != '\0') {
        writeToSerialPort(base, *str);
        ++str;
    }
}

__PRIVILEGED_CODE
char readFromSerialPort(uint16_t base) {
    // Wait until data is available
    while (!isDataAvailable(base));
    
    // Read and return the character from the data port
    return inByte(SERIAL_DATA_PORT(base));
}
#include <kprint.h>
__PRIVILEGED_CODE
void readStringFromSerialPort(uint16_t base, char* buffer, size_t maxLength) {
    size_t index = 0;

    kuPrint("readStringFromSerialPort: Waiting for data...\n");

    // Read until we reach maxLength - 1 or encounter a newline or carriage return
    while (index < maxLength - 1) {
        char chr = readFromSerialPort(base);
        
        kuPrint("readStringFromSerialPort: Received character '%c' (%d)\n", chr, (int)chr);
        
        // Check for carriage return (Enter key)
        if (chr == '\r') {
            // Optionally convert '\r' to '\n'
            chr = '\n';

            // Break to signal end of input
            buffer[index++] = chr;
            break;
        }

        // Store the character
        buffer[index++] = chr;
    }

    // Null-terminate the string
    buffer[index] = '\0';
    kuPrint("readStringFromSerialPort: Completed read, buffer = '%s'\n", buffer);
}
