#include "console.h"
#include <drivers/serial/serial_driver.h>
#include <kstring.h>

void Console::connectToSerial(uint16_t port) {
    m_serialPort = port;
}

size_t Console::read(char* buffer, size_t len) {
    while (_isBufferEmpty()) {
        asm volatile ("nop");
    }

    size_t bytesRead = 0;

    while (bytesRead < len && !_isBufferEmpty()) {
        // Read data from the circular buffer
        buffer[bytesRead++] = m_dataBuffer[m_readPtr];
        m_readPtr = (m_readPtr + 1) % CONSOLE_BUFFER_SIZE;  // Increment and wrap around if needed
    }

    return bytesRead;
}

void Console::write(const char* buffer) {
    if (_isBufferFull()) {
        return;
    }

    // Write to the serial port if connected
    if (m_serialPort) {
        SerialDriver::writePort(m_serialPort, buffer);
    }

    size_t bufferLen = strlen(buffer);
    for (size_t i = 0; i < bufferLen; i++) {
        // If the buffer is full, stop writing
        if (_isBufferFull()) {
            break;
        }

        // Write data to the circular buffer
        m_dataBuffer[m_writePtr] = buffer[i];
        m_writePtr = (m_writePtr + 1) % CONSOLE_BUFFER_SIZE;  // Increment and wrap around if needed
    }
}

bool Console::_isBufferEmpty() const {
    return m_readPtr == m_writePtr;
}

bool Console::_isBufferFull() const {
    return ((m_writePtr + 1) % CONSOLE_BUFFER_SIZE) == m_readPtr;
}
