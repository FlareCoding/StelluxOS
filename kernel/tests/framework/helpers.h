#ifndef STELLUX_TESTS_FRAMEWORK_HELPERS_H
#define STELLUX_TESTS_FRAMEWORK_HELPERS_H

#include "common/types.h"
#include "clock/clock.h"
#include "sync/atomic.h"

namespace test_helpers {

// Wall-clock bound: iteration counts vary ~100x across hosts and emulators.
constexpr uint64_t SPIN_TIMEOUT_NS = 20000000000ULL; // 20s

inline bool spin_wait(const sync::atomic<uint32_t>& flag) {
    uint64_t deadline = clock::now_ns() + SPIN_TIMEOUT_NS;
    while (!flag.load_acquire()) {
        if (clock::now_ns() > deadline) return false;
    }
    return true;
}

inline bool spin_wait_ge(const sync::atomic<uint32_t>& value, uint32_t target) {
    uint64_t deadline = clock::now_ns() + SPIN_TIMEOUT_NS;
    while (value.load_acquire() < target) {
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
