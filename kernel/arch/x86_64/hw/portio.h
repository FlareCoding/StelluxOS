#ifndef STELLUX_ARCH_X86_64_HW_PORTIO_H
#define STELLUX_ARCH_X86_64_HW_PORTIO_H

#include "types.h"

namespace portio {

/**
 * Read 8-bit value from I/O port.
 */
inline uint8_t in8(uint16_t port) {
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/**
 * Read 16-bit value from I/O port.
 */
inline uint16_t in16(uint16_t port) {
    uint16_t val;
    asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/**
 * Read 32-bit value from I/O port.
 */
inline uint32_t in32(uint16_t port) {
    uint32_t val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/**
 * Write 8-bit value to I/O port.
 */
inline void out8(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * Write 16-bit value to I/O port.
 */
inline void out16(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * Write 32-bit value to I/O port.
 */
inline void out32(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

} // namespace portio

#endif // STELLUX_ARCH_X86_64_HW_PORTIO_H
