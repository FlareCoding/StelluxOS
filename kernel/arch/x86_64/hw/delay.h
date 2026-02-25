#ifndef STELLUX_ARCH_X86_64_HW_DELAY_H
#define STELLUX_ARCH_X86_64_HW_DELAY_H

#include "common/types.h"
#include "hw/portio.h"
#include "hw/cpu.h"

namespace delay {

constexpr uint32_t PIT_FREQ     = 1193182;
constexpr uint16_t PIT_CTRL     = 0x43;
constexpr uint16_t PIT_CH2_DATA = 0x42;
constexpr uint16_t PORT_B       = 0x61;
constexpr uint8_t  PORT_B_GATE  = 0x01;
constexpr uint8_t  PORT_B_SPKR  = 0x02;
constexpr uint8_t  PORT_B_OUT2  = 0x20;
constexpr uint32_t MAX_CHUNK_MS = 50;

/**
 * Busy-wait for the given number of milliseconds using PIT channel 2
 * in one-shot mode. Chunks delays > 50ms to avoid 16-bit counter overflow.
 * IRQs are disabled for the duration of each chunk (~50ms max).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void pit_ms(uint32_t ms) {
    if (ms == 0) return;

    while (ms > 0) {
        uint32_t chunk = (ms > MAX_CHUNK_MS) ? MAX_CHUNK_MS : ms;
        uint16_t count = static_cast<uint16_t>((PIT_FREQ * chunk) / 1000);
        if (count == 0) count = 1;

        uint64_t flags = cpu::irq_save();

        uint8_t port_b_orig = portio::in8(PORT_B);

        portio::out8(PORT_B, (port_b_orig & ~PORT_B_SPKR) & ~PORT_B_GATE);

        portio::out8(PIT_CTRL, 0xB0);
        portio::out8(PIT_CH2_DATA, static_cast<uint8_t>(count & 0xFF));
        portio::out8(PIT_CH2_DATA, static_cast<uint8_t>(count >> 8));

        portio::out8(PORT_B, (port_b_orig & ~PORT_B_SPKR) | PORT_B_GATE);

        while ((portio::in8(PORT_B) & PORT_B_OUT2) == 0) {
            cpu::relax();
        }

        portio::out8(PORT_B, port_b_orig);

        cpu::irq_restore(flags);

        ms -= chunk;
    }
}

} // namespace delay

#endif // STELLUX_ARCH_X86_64_HW_DELAY_H
