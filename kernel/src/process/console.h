#ifndef CONSOLE_H
#define CONSOLE_H
#include <ktypes.h>

#define CONSOLE_BUFFER_SIZE 1024

class Console {
public:
    Console() = default;
    ~Console() = default;

    void connectToSerial(uint16_t port);
    __force_inline__ uint16_t checkSerialConnection() const { return m_serialPort; }

    size_t read(char* buffer, size_t len);
    void write(const char* buffer);

private:
    char m_dataBuffer[CONSOLE_BUFFER_SIZE];
    size_t m_readPtr = 0;
    size_t m_writePtr = 0;

    uint16_t m_serialPort = 0;

    bool _isBufferEmpty() const;
    bool _isBufferFull() const;
};

#endif
