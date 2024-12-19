#include <unit_tests/unit_tests.h>
#include <dynpriv/dynpriv.h>
#include <memory/tlb.h>

using namespace dynpriv;

__PRIVILEGED_DATA uint32_t g_privileged_test_cookie;

// Drop privileged if already in elevated/privileged state
#define DYNPRIV_TEST_BEGIN \
    bool was_elevated = is_elevated(); \
    if (was_elevated) { \
        lower(); \
    }

// If the test was executed from a privileged
// state, acquire hw privilege back.
#define DYNPRIV_TEST_END \
    if (was_elevated) { \
        elevate(); \
    }

// Test a simple elevate-lower pair in a loop
DECLARE_UNIT_TEST("elevate-lower loop", test_elevate_lower_loop) {
    DYNPRIV_TEST_BEGIN

    const size_t iterations = 10000;
    for (size_t i = 0; i < iterations; i++) {
        elevate();
        lower();
    }

    DYNPRIV_TEST_END
    return UNIT_TEST_SUCCESS;
}

// Test the ability to execute privileged instructions
DECLARE_UNIT_TEST("elevated read cr3", test_elevated_read_cr3) {
    DYNPRIV_TEST_BEGIN

    uint64_t cr3;
    elevate();
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    serial::printf("cr3: 0x%llx\n", cr3);
    lower();

    DYNPRIV_TEST_END
    return UNIT_TEST_SUCCESS;
}

// Test the ability to execute privileged
// instructions with a RUN_ELEVATED macro.
DECLARE_UNIT_TEST("elevated read cr3 with RUN_ELEVATED", test_elevated_read_cr3_with_run_elevated_macro) {
    DYNPRIV_TEST_BEGIN

    uint64_t cr3;
    RUN_ELEVATED(
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        serial::printf("cr3: 0x%llx\n", cr3);
    );

    DYNPRIV_TEST_END
    return UNIT_TEST_SUCCESS;
}

// Test the ability to execute a privileged function
DECLARE_UNIT_TEST("elevated run privileged function", test_elevated_run_privileged_function) {
    DYNPRIV_TEST_BEGIN

    RUN_ELEVATED(
        paging::tlb_flush_all();
    );

    DYNPRIV_TEST_END
    return UNIT_TEST_SUCCESS;
}

// Test the ability to access privileged data
DECLARE_UNIT_TEST("elevated access privileged data", test_elevated_access_privileged_data) {
    DYNPRIV_TEST_BEGIN

    RUN_ELEVATED(
        g_privileged_test_cookie = 1;
        serial::printf("privileged cookie: %u\n", g_privileged_test_cookie);
        g_privileged_test_cookie = 0;
    );

    DYNPRIV_TEST_END
    return UNIT_TEST_SUCCESS;
}
