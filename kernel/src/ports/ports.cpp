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

__PRIVILEGED_CODE
void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %w0, %w1" : : "a" (val), "Nd" (port));
}

__PRIVILEGED_CODE
uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %w1, %w0" : "=a" (ret) : "Nd" (port));
    return ret;
}

__PRIVILEGED_CODE
void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

__PRIVILEGED_CODE
uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
