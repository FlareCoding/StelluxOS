#ifndef STELLUX_TESTS_FRAMEWORK_HELPERS_H
#define STELLUX_TESTS_FRAMEWORK_HELPERS_H

#include "common/types.h"
#include "clock/clock.h"

namespace test_helpers {

// Wall-clock bound: iteration counts vary ~100x across hosts and emulators.
constexpr uint64_t SPIN_TIMEOUT_NS = 20000000000ULL; // 20s

inline bool spin_wait(volatile uint32_t* flag) {
    uint64_t deadline = clock::now_ns() + SPIN_TIMEOUT_NS;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (clock::now_ns() > deadline) return false;
    }
    return true;
}

inline bool spin_wait_ge(volatile uint32_t* value, uint32_t target) {
    uint64_t deadline = clock::now_ns() + SPIN_TIMEOUT_NS;
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < target) {
        if (clock::now_ns() > deadline) return false;
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
