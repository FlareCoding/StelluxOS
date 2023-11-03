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
}

__PRIVILEGED_CODE
bool isTransmitQueueEmpty(uint16_t base) {
    return inByte(SERIAL_LINE_STATUS_PORT(base)) & SERIAL_LINE_STATUS_THR_EMPTY;
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
