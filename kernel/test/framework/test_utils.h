#ifndef STELLUX_TEST_FRAMEWORK_TEST_UTILS_H
#define STELLUX_TEST_FRAMEWORK_TEST_UTILS_H

#include "test_runner.h"

namespace test {

inline uint64_t rand_u64(context& ctx) {
    return random_next_u64(ctx);
}

inline uint32_t rand_u32(context& ctx) {
    return static_cast<uint32_t>(random_next_u64(ctx) & 0xFFFFFFFFU);
}

inline uint64_t rand_range(context& ctx, uint64_t min, uint64_t max) {
    if (max <= min) {
        return min;
    }
    uint64_t span = max - min + 1;
    return min + (random_next_u64(ctx) % span);
}

} // namespace test

#endif // STELLUX_TEST_FRAMEWORK_TEST_UTILS_H
