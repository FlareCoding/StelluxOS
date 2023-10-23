#include "ports.h"

__PRIVILEGED_CODE
void outByte(uint16_t port, uint8_t value) {
   __asm__ volatile ("outb %1, %0" : : "dN" (port), "a" (value));
}

__PRIVILEGED_CODE
uint8_t inByte(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a" (ret) : "dN" (port));
    return ret;
}
