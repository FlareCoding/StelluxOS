#include "clock/clock.h"
#include "hw/tsc.h"
#include "hw/portio.h"
#include "hw/mmio.h"
#include "hw/cpu.h"
#include "hw/rtc.h"
#include "cpu/features.h"
#include "common/logging.h"

namespace clock {

static uint64_t g_tsc_freq;
static uint64_t g_mult;
static uint32_t g_shift;
static bool g_calibrated;
static uint64_t g_boot_realtime_ns;

constexpr uint64_t NS_PER_SEC = 1000000000ULL;

// PIT constants (same as hwtimer calibration)
constexpr uint16_t PIT_CTRL     = 0x43;
constexpr uint16_t PIT_CH2_DATA = 0x42;
constexpr uint16_t PORT_B       = 0x61;
constexpr uint8_t  PORT_B_GATE  = 0x01;
constexpr uint8_t  PORT_B_SPKR  = 0x02;
constexpr uint8_t  PORT_B_OUT2  = 0x20;
constexpr uint16_t PIT_10MS     = 11932;

/**
 * Compute mult and shift such that:
 *   ns = (ticks * mult) >> shift
 * where mult / 2^shift approximates NS_PER_SEC / freq.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void compute_mult_shift(uint64_t freq,
                                                  uint64_t* out_mult,
                                                  uint32_t* out_shift) {
    // Target: find largest shift where mult fits in 64 bits
    // mult = (NS_PER_SEC << shift) / freq
    for (uint32_t s = 32; s > 0; s--) {
        uint64_t m = (NS_PER_SEC << s) / freq;
        if (m != 0 && m <= 0xFFFFFFFFFFFFFFFFULL) {
            *out_mult = m;
            *out_shift = s;
            return;
        }
    }
    *out_mult = NS_PER_SEC / freq;
    *out_shift = 0;
}

/**
 * Calibrate TSC frequency using PIT channel 2 (~10ms gate).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint64_t calibrate_tsc_pit() {
    uint8_t port_b_orig = portio::in8(PORT_B);

    portio::out8(PORT_B, (port_b_orig & ~PORT_B_SPKR) & ~PORT_B_GATE);

    portio::out8(PIT_CTRL, 0xB0);
    portio::out8(PIT_CH2_DATA, PIT_10MS & 0xFF);
    portio::out8(PIT_CH2_DATA, PIT_10MS >> 8);

    portio::out8(PORT_B, (port_b_orig & ~PORT_B_SPKR) | PORT_B_GATE);
    uint64_t tsc_start = tsc::rdtsc();

    while ((portio::in8(PORT_B) & PORT_B_OUT2) == 0) {
        cpu::relax();
    }

    uint64_t tsc_end = tsc::rdtsc();
    portio::out8(PORT_B, port_b_orig);

    return (tsc_end - tsc_start) * 100;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    if (!cpu::has(cpu::TSC)) {
        log::error("clock: CPU does not support TSC");
        return ERR;
    }

    g_tsc_freq = calibrate_tsc_pit();
    if (g_tsc_freq == 0) {
        log::error("clock: TSC calibration failed");
        return ERR;
    }

    compute_mult_shift(g_tsc_freq, &g_mult, &g_shift);
    g_boot_realtime_ns = rtc::boot_unix_ns();
    __atomic_store_n(&g_calibrated, true, __ATOMIC_RELEASE);

    log::info("clock: TSC freq=%lu Hz, mult=%lu shift=%u%s",
              g_tsc_freq, g_mult, g_shift,
              cpu::has(cpu::INVARIANT_TSC) ? " (invariant)" : "");

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap() {
    if (!__atomic_load_n(&g_calibrated, __ATOMIC_ACQUIRE)) {
        return ERR;
    }
    return OK;
}

uint64_t now_ns() {
    if (!__atomic_load_n(&g_calibrated, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    uint64_t ticks = tsc::rdtsc();
    return static_cast<uint64_t>(
        (static_cast<unsigned __int128>(ticks) * g_mult) >> g_shift
    );
}

uint64_t freq_hz() {
    return g_tsc_freq;
}

uint64_t boot_realtime_ns() {
    return g_boot_realtime_ns;
}

void set_boot_realtime_ns(uint64_t ns) {
    g_boot_realtime_ns = ns;
}

} // namespace clock
