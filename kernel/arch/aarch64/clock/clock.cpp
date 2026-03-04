#include "clock/clock.h"
#include "hwtimer/hwtimer_arch.h"
#include "hw/rtc.h"
#include "common/logging.h"

namespace clock {

static uint64_t g_cnt_freq;
static uint64_t g_mult;
static uint32_t g_shift;
static bool g_calibrated;
static uint64_t g_boot_realtime_ns;

constexpr uint64_t NS_PER_SEC = 1000000000ULL;

/**
 * Compute mult and shift such that:
 *   ns = (ticks * mult) >> shift
 * where mult / 2^shift approximates NS_PER_SEC / freq.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void compute_mult_shift(uint64_t freq,
                                                  uint64_t* out_mult,
                                                  uint32_t* out_shift) {
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
 * Enable EL0 access to the virtual counter (CNTVCT_EL0)
 * by setting CNTKCTL_EL1.EL0VCTEN (bit 1).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void enable_el0_counter_access() {
    uint64_t val;
    asm volatile("mrs %0, CNTKCTL_EL1" : "=r"(val));
    val |= (1 << 1); // EL0VCTEN
    asm volatile("msr CNTKCTL_EL1, %0" : : "r"(val));
    asm volatile("isb");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    g_cnt_freq = hwtimer::read_cntfrq();
    if (g_cnt_freq == 0) {
        log::error("clock: CNTFRQ_EL0 is zero");
        return ERR;
    }

    compute_mult_shift(g_cnt_freq, &g_mult, &g_shift);
    g_boot_realtime_ns = rtc::boot_unix_ns();
    enable_el0_counter_access();
    __atomic_store_n(&g_calibrated, true, __ATOMIC_RELEASE);

    log::info("clock: CNTFRQ=%lu Hz, mult=%lu shift=%u",
              g_cnt_freq, g_mult, g_shift);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap() {
    enable_el0_counter_access();
    if (!__atomic_load_n(&g_calibrated, __ATOMIC_ACQUIRE)) {
        return ERR;
    }
    return OK;
}

uint64_t now_ns() {
    if (!__atomic_load_n(&g_calibrated, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    uint64_t ticks = hwtimer::read_cntvct();
    return static_cast<uint64_t>(
        (static_cast<unsigned __int128>(ticks) * g_mult) >> g_shift
    );
}

uint64_t freq_hz() {
    return g_cnt_freq;
}

uint64_t boot_realtime_ns() {
    return g_boot_realtime_ns;
}

void set_boot_realtime_ns(uint64_t ns) {
    g_boot_realtime_ns = ns;
}

} // namespace clock
