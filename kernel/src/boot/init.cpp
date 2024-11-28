#include <serial/serial.h>

EXTERN_C void init(unsigned int magic, void* mbi) {
    serial::init_port(SERIAL_PORT_BASE_COM1);
    serial::write(SERIAL_PORT_BASE_COM1, "Hello\n");

    while (true) { asm volatile ("hlt"); }
}