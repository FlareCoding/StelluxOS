#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_REGISTERS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_REGISTERS_H

#include "common/types.h"
#include "hw/mmio.h"

namespace drivers::nvidia {

/**
 * A view of the card's 32-bit registers at a base address, such as the
 * driver's BAR0 mapping. The view owns nothing, so the mapping must outlive it.
 * Only a non-const view writes, so a step given a const view writes no
 * register through it.
 */
class registers {
public:
    explicit registers(uintptr_t base) : m_base(base) {}

    // `offset` counts bytes from the base and must be a multiple of 4
    uint32_t read(uint32_t offset) const { return mmio::read32(m_base + offset); }
    void write(uint32_t offset, uint32_t value) { mmio::write32(m_base + offset, value); }

private:
    uintptr_t m_base;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_REGISTERS_H
