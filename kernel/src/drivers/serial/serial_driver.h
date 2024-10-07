#ifndef SERIAL_DRIVER_H
#define SERIAL_DRIVER_H
#include <ports/serial.h>

class SerialDriver {
public:
    static void init();

    static void writePort(uint16_t port, const char* buffer);
    static char readPort(uint16_t port);
};

#endif
