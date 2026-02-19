#ifndef STELLUX_TEST_FRAMEWORK_TEST_REGISTRY_H
#define STELLUX_TEST_FRAMEWORK_TEST_REGISTRY_H

#include "common/types.h"

namespace test {

constexpr uint32_t ABI_VERSION = 1;

enum class phase : uint8_t {
    early = 0,
    post_mm = 1,
};

struct context;

using test_body_fn = void (*)(context& ctx);
using suite_hook_fn = void (*)(context& ctx);

struct suite_desc {
    uint32_t abi_version;
    const char* name;
    phase run_phase;
    suite_hook_fn before_each;
    suite_hook_fn after_each;
};

struct case_desc {
    uint32_t abi_version;
    const suite_desc* suite;
    const char* name;
    test_body_fn body;
};

} // namespace test

#endif // STELLUX_TEST_FRAMEWORK_TEST_REGISTRY_H
