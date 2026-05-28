#ifndef STELLUX_ARCH_AARCH64_TRACE_TS_H
#define STELLUX_ARCH_AARCH64_TRACE_TS_H

#include "common/types.h"

namespace trace {

constexpr uint32_t ARCH_ID = 2;

inline uint64_t timestamp() {
    uint64_t ts;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(ts));
    return ts;
}

} // namespace trace

#endif
