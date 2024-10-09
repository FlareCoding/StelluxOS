#ifndef CONSOLE_H
#define CONSOLE_H
#include <sync.h>

class Console {
public:
    Console() = default;
    ~Console() = default;

    void connectOutputToSerial(uint16_t port);
    void connectInputToSerial(uint16_t port);

    __force_inline__ uint16_t checkInputSerialConnection() const { return m_inputSerialPort; }

    // Writes data to the console
    void write(const char* data, size_t length);
    void write(const char* data);

    // Reads data from the console's input buffer
    size_t read(char* buffer, size_t length);

    // Reads data from the console's input
    // buffer up to the newline character.
    size_t readLine(char* buffer, size_t length);

    // Input devices call this to post input data
    void postInput(const char* data, size_t length);

private:
    uint16_t m_outputSerialPort = 0;
    uint16_t m_inputSerialPort = 0;

    // Input buffer (ring buffer)
    static const size_t INPUT_BUFFER_SIZE = 1024;

    char m_inputBuffer[INPUT_BUFFER_SIZE];
    Atomic<size_t> m_inputBufferHead;
    Atomic<size_t> m_inputBufferTail;
};

void setActiveConsole(Console* console);
Console* getActiveConsole();

#endif
