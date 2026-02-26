#ifndef STELLUX_TESTS_FRAMEWORK_HELPERS_H
#define STELLUX_TESTS_FRAMEWORK_HELPERS_H

#include "common/types.h"

namespace test_helpers {

constexpr uint64_t SPIN_TIMEOUT = 100000000;

inline bool spin_wait(volatile uint32_t* flag) {
    uint64_t spins = 0;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (++spins > SPIN_TIMEOUT) return false;
    }
    return true;
}

inline bool spin_wait_ge(volatile uint32_t* value, uint32_t target) {
    uint64_t spins = 0;
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < target) {
        if (++spins > SPIN_TIMEOUT) return false;
    }
    return true;
}

inline void brief_delay() {
    uint64_t i = 0;
    while (i < 5000000) {
        asm volatile("" : "+r"(i));
        i++;
    }
}

} // namespace test_helpers

#endif // STELLUX_TESTS_FRAMEWORK_HELPERS_H
