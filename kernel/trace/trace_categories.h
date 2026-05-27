#ifndef STELLUX_TRACE_CATEGORIES_H
#define STELLUX_TRACE_CATEGORIES_H

#include "common/types.h"

namespace trace {

enum category : uint16_t {
    none      = 0,
    boot      = 1 << 0,
    syscall   = 1 << 1,
    sched     = 1 << 2,
    mm        = 1 << 3,
    irq       = 1 << 4,
    fs        = 1 << 5,
    all       = 0xffff,
};

inline category operator|(category a, category b) {
    return static_cast<category>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline category operator&(category a, category b) {
    return static_cast<category>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}

} // namespace trace

#endif // STELLUX_TRACE_CATEGORIES_H
