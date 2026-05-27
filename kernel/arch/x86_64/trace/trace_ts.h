#ifndef STELLUX_ARCH_X86_64_TRACE_TS_H
#define STELLUX_ARCH_X86_64_TRACE_TS_H

#include "common/types.h"

namespace trace {

inline uint64_t timestamp() {
    uint32_t lo, hi, _aux;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(_aux));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

} // namespace trace

#endif

