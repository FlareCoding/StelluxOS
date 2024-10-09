#include "console.h"
#include <drivers/serial/serial_driver.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <kstring.h>

Console* g_globalActiveConsole = nullptr;

void Console::connectOutputToSerial(uint16_t port) {
    m_outputSerialPort = port;
}

void Console::connectInputToSerial(uint16_t port) {
    m_inputSerialPort = port;
}

void Console::write(const char* data, size_t length) {
    if (m_outputSerialPort != 0) {
        char serialBuf[length + 1] = { 0 };
        memcpy(serialBuf, data, length);
        serialBuf[length] = NULL;

        SerialDriver::writePort(m_outputSerialPort, serialBuf);
    }
}

void Console::write(const char* data) {
    size_t len = strlen(data);
    write(data, len);
}

size_t Console::read(char* buffer, size_t length) {
    size_t tail = m_inputBufferTail.load(ATOMIC_MEMORY_ORDER_RELAXED);
    size_t head = m_inputBufferHead.load(ATOMIC_MEMORY_ORDER_ACQUIRE);

    size_t available;
    if (head >= tail) {
        available = head - tail;
    } else {
        available = INPUT_BUFFER_SIZE - (tail - head);
    }

    size_t toRead = (length < available) ? length : available;

    for (size_t i = 0; i < toRead; ++i) {
        buffer[i] = m_inputBuffer[(tail + i) % INPUT_BUFFER_SIZE];
    }

    // Update the tail index after reading
    m_inputBufferTail.store((tail + toRead) % INPUT_BUFFER_SIZE, ATOMIC_MEMORY_ORDER_RELEASE);

    return toRead;
}

size_t Console::readLine(char* buffer, size_t length) {
    size_t totalRead = 0;

    while (true) {
        size_t tail = m_inputBufferTail.load(ATOMIC_MEMORY_ORDER_RELAXED);
        size_t head = m_inputBufferHead.load(ATOMIC_MEMORY_ORDER_ACQUIRE);

        // Calculate available data
        size_t available;
        if (head >= tail) {
            available = head - tail;
        } else {
            available = INPUT_BUFFER_SIZE - (tail - head);
        }

        // Read available data up to the newline character
        size_t i = 0;
        for (; i < available && totalRead < length; ++i) {
            char c = m_inputBuffer[(tail + i) % INPUT_BUFFER_SIZE];
            buffer[totalRead++] = c;

            // Check for newline character
            if (c == '\n') {
                // Exclude the newline character from the buffer
                totalRead--;
                buffer[totalRead] = 0;

                m_inputBufferTail.store((tail + i + 1) % INPUT_BUFFER_SIZE, ATOMIC_MEMORY_ORDER_RELEASE);
                return totalRead;
            }
        }

        // Update the tail index after reading
        m_inputBufferTail.store((tail + i) % INPUT_BUFFER_SIZE, ATOMIC_MEMORY_ORDER_RELEASE);

        // If we haven't found a newline yet, continue spinning
        if (totalRead < length) {
            // Busy-wait loop
            msleep(1);
        } else {
            // Buffer is full but no newline found; return what we have
            return totalRead;
        }
    }
}

void Console::postInput(const char* data, size_t length) {
    size_t head = m_inputBufferHead.load(ATOMIC_MEMORY_ORDER_RELAXED);
    size_t tail = m_inputBufferTail.load(ATOMIC_MEMORY_ORDER_ACQUIRE);

    size_t space;
    if (head >= tail) {
        space = INPUT_BUFFER_SIZE - (head - tail);
    } else {
        space = tail - head;
    }

    if (space > length) {
        for (size_t i = 0; i < length; ++i) {
            m_inputBuffer[(head + i) % INPUT_BUFFER_SIZE] = data[i];
        }
        // Update the head index after writing
        m_inputBufferHead.store((head + length) % INPUT_BUFFER_SIZE, ATOMIC_MEMORY_ORDER_RELEASE);
    } else {
        // Handle buffer overflow (e.g., drop data or overwrite old data)
    }
}

void setActiveConsole(Console* console) {
    g_globalActiveConsole = console;
}

Console* getActiveConsole() {
    return g_globalActiveConsole;
}
