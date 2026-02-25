#include "hwtimer/hwtimer.h"
#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "defs/vectors.h"
#include "hw/portio.h"
#include "hw/mmio.h"
#include "hw/cpu.h"
#include "common/logging.h"

namespace hwtimer {

__PRIVILEGED_BSS static uint64_t g_lapic_freq;

// PIT constants
constexpr uint16_t PIT_CTRL     = 0x43;
constexpr uint16_t PIT_CH2_DATA = 0x42;
constexpr uint16_t PORT_B       = 0x61;
constexpr uint8_t  PORT_B_GATE  = 0x01;
constexpr uint8_t  PORT_B_SPKR  = 0x02;
constexpr uint8_t  PORT_B_OUT2  = 0x20;
constexpr uint16_t PIT_10MS     = 11932; // ~10ms at 1,193,182 Hz

/**
 * Calibrate LAPIC timer frequency using PIT channel 2.
 * Returns ticks per second, or 0 on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint64_t calibrate_lapic() {
    uintptr_t lapic = irq::get_lapic_va();

    // Mask LAPIC timer during calibration
    uint32_t lvt = mmio::read32(lapic + irq::LAPIC_LVT_TIMER);
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER, lvt | irq::LVT_MASKED);

    // Set divider to 1
    mmio::write32(lapic + irq::LAPIC_TIMER_DCR, 0x7);

    // Save port 0x61 state
    uint8_t port_b_orig = portio::in8(PORT_B);

    // Gate off, speaker off
    portio::out8(PORT_B, (port_b_orig & ~PORT_B_SPKR) & ~PORT_B_GATE);

    // Program PIT channel 2: mode 0 (one-shot), lobyte/hibyte, binary
    portio::out8(PIT_CTRL, 0xB0);
    portio::out8(PIT_CH2_DATA, PIT_10MS & 0xFF);
    portio::out8(PIT_CH2_DATA, PIT_10MS >> 8);

    // Gate on (PIT starts counting), start LAPIC timer
    portio::out8(PORT_B, (port_b_orig & ~PORT_B_SPKR) | PORT_B_GATE);
    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, 0xFFFFFFFF);

    // Wait for PIT OUT2 to go high (terminal count)
    while ((portio::in8(PORT_B) & PORT_B_OUT2) == 0) {
        cpu::relax();
    }

    // Read elapsed LAPIC ticks
    uint32_t current = mmio::read32(lapic + irq::LAPIC_TIMER_CCR);
    uint32_t elapsed = 0xFFFFFFFF - current;

    // Stop LAPIC timer
    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, 0);

    // Restore port 0x61
    portio::out8(PORT_B, port_b_orig);

    // elapsed ticks in ~10ms → frequency = elapsed * 100
    return static_cast<uint64_t>(elapsed) * 100;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(uint32_t hz) {
    uintptr_t lapic = irq::get_lapic_va();
    if (!lapic) {
        log::error("hwtimer: LAPIC not initialized");
        return ERR;
    }

    g_lapic_freq = calibrate_lapic();
    if (g_lapic_freq == 0) {
        log::error("hwtimer: LAPIC calibration failed");
        return ERR;
    }

    uint32_t count = static_cast<uint32_t>(g_lapic_freq / hz);

    // Configure LVT Timer: periodic mode, VEC_TIMER, masked during setup
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER,
                  irq::LVT_MASKED | irq::LVT_PERIODIC | x86::VEC_TIMER);

    // Set divider to 1 (same as calibration)
    mmio::write32(lapic + irq::LAPIC_TIMER_DCR, 0x7);

    // Write initial count (starts the timer)
    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, count);

    // Unmask LVT Timer
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER,
                  irq::LVT_PERIODIC | x86::VEC_TIMER);

    // Enable CPU interrupts so timer IRQs can be delivered
    cpu::irq_enable();

    log::info("hwtimer: LAPIC freq=%lu Hz, periodic at %u Hz (count=%u)",
              g_lapic_freq, hz, count);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap(uint32_t hz) {
    uintptr_t lapic = irq::get_lapic_va();
    if (!lapic || g_lapic_freq == 0) {
        return ERR;
    }

    uint32_t count = static_cast<uint32_t>(g_lapic_freq / hz);

    mmio::write32(lapic + irq::LAPIC_LVT_TIMER,
                  irq::LVT_MASKED | irq::LVT_PERIODIC | x86::VEC_TIMER);

    mmio::write32(lapic + irq::LAPIC_TIMER_DCR, 0x7);

    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, count);

    mmio::write32(lapic + irq::LAPIC_LVT_TIMER,
                  irq::LVT_PERIODIC | x86::VEC_TIMER);

    cpu::irq_enable();

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void stop() {
    uintptr_t lapic = irq::get_lapic_va();
    if (!lapic) return;

    // Mask LVT Timer
    uint32_t lvt = mmio::read32(lapic + irq::LAPIC_LVT_TIMER);
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER, lvt | irq::LVT_MASKED);

    // Stop counting
    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, 0);
}

} // namespace hwtimer
