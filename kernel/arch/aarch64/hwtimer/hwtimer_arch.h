#ifndef STELLUX_AARCH64_HWTIMER_HWTIMER_ARCH_H
#define STELLUX_AARCH64_HWTIMER_HWTIMER_ARCH_H

#include "common/types.h"

namespace hwtimer {

constexpr uint32_t TIMER_PPI = 27; // Virtual timer PPI (GIC interrupt ID)

inline uint32_t read_cntfrq() {
    uint64_t v;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return static_cast<uint32_t>(v);
}

inline uint64_t read_cntvct() {
    uint64_t v;
    asm volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}

inline uint64_t read_cntpct() {
    uint64_t v;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(v));
    return v;
}

inline void write_cntv_tval(uint32_t val) {
    asm volatile("msr CNTV_TVAL_EL0, %0" : : "r"(static_cast<uint64_t>(val)));
}

inline void write_cntv_cval(uint64_t val) {
    asm volatile("msr CNTV_CVAL_EL0, %0" : : "r"(val));
}

inline void write_cntv_ctl(uint32_t val) {
    asm volatile("msr CNTV_CTL_EL0, %0" : : "r"(static_cast<uint64_t>(val)));
}

} // namespace hwtimer

#endif // STELLUX_AARCH64_HWTIMER_HWTIMER_ARCH_H
