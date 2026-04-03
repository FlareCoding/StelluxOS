#ifndef STELLUX_HW_MMIO_H
#define STELLUX_HW_MMIO_H

#include "types.h"
#include "hw/barrier.h"

namespace mmio {

/**
 * Read 8-bit value from memory-mapped register.
 */
inline uint8_t read8(uintptr_t addr) {
    uint8_t val = *reinterpret_cast<volatile uint8_t*>(addr);
    barrier::io_read();
    return val;
}

/**
 * Read 16-bit value from memory-mapped register.
 */
inline uint16_t read16(uintptr_t addr) {
    uint16_t val = *reinterpret_cast<volatile uint16_t*>(addr);
    barrier::io_read();
    return val;
}

/**
 * Read 32-bit value from memory-mapped register.
 */
inline uint32_t read32(uintptr_t addr) {
    uint32_t val = *reinterpret_cast<volatile uint32_t*>(addr);
    barrier::io_read();
    return val;
}

/**
 * Write 8-bit value to memory-mapped register.
 */
inline void write8(uintptr_t addr, uint8_t val) {
    barrier::io_write();
    *reinterpret_cast<volatile uint8_t*>(addr) = val;
}

/**
 * Write 16-bit value to memory-mapped register.
 */
inline void write16(uintptr_t addr, uint16_t val) {
    barrier::io_write();
    *reinterpret_cast<volatile uint16_t*>(addr) = val;
}

/**
 * Write 32-bit value to memory-mapped register.
 */
inline void write32(uintptr_t addr, uint32_t val) {
    barrier::io_write();
    *reinterpret_cast<volatile uint32_t*>(addr) = val;
}

} // namespace mmio

#endif // STELLUX_HW_MMIO_H
