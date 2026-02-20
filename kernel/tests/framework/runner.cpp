#include "runner.h"
#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/varargs.h"
#include "common/string.h"

extern "C" {
extern const stlx_test::test_entry __stlx_unit_test_start[];
extern const stlx_test::test_entry __stlx_unit_test_end[];
extern const stlx_test::suite_hooks __stlx_unit_test_hooks_start[];
extern const stlx_test::suite_hooks __stlx_unit_test_hooks_end[];
}

namespace stlx_test {

void print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log::vraw(fmt, args);
    va_end(args);
}

namespace detail {

void report_fail(const char* file, int line, const char* macro, const char* expr) {
    if (expr) {
        log::raw("  # %s failed at %s:%d", macro, file, line);
        log::raw("  # expression: %s", expr);
    } else {
        log::raw("  # %s failed at %s:%d", macro, file, line);
    }
}

void report_values_uint(const char* a_str, const char* b_str,
                        uint64_t lhs, uint64_t rhs) {
    log::raw("  # left:  %s = %lu", a_str, lhs);
    log::raw("  # right: %s = %lu", b_str, rhs);
}

void report_values_ptr(const char* a_str, const char* b_str,
                       uintptr_t lhs, uintptr_t rhs) {
    log::raw("  # left:  %s = 0x%016lx", a_str, lhs);
    log::raw("  # right: %s = 0x%016lx", b_str, rhs);
}

void report_values_bool(const char* a_str, const char* b_str,
                        bool lhs, bool rhs) {
    log::raw("  # left:  %s = %s", a_str, lhs ? "true" : "false");
    log::raw("  # right: %s = %s", b_str, rhs ? "true" : "false");
}

void report_values_str(const char* a_str, const char* b_str,
                       const char* lhs, const char* rhs) {
    log::raw("  # left:  %s = \"%s\"", a_str, lhs ? lhs : "(null)");
    log::raw("  # right: %s = \"%s\"", b_str, rhs ? rhs : "(null)");
}

} // namespace detail

// Find a hook function for a given suite and hook type
static hook_fn find_hook(const char* suite_name, hook_type type) {
    for (const auto* h = __stlx_unit_test_hooks_start;
         h < __stlx_unit_test_hooks_end; h++) {
        if (h->type == type && string::strcmp(h->suite_name, suite_name) == 0) {
            return h->fn;
        }
    }
    return nullptr;
}

// Simple in-place insertion sort of test entries by suite_name
static void sort_by_suite(test_entry* begin, test_entry* end) {
    const size_t n = static_cast<size_t>(end - begin);
    for (size_t i = 1; i < n; i++) {
        test_entry key = begin[i];
        size_t j = i;
        while (j > 0 && string::strcmp(begin[j - 1].suite_name, key.suite_name) > 0) {
            begin[j] = begin[j - 1];
            j--;
        }
        begin[j] = key;
    }
}

int32_t run_all() {
    auto* tests_begin = const_cast<test_entry*>(__stlx_unit_test_start);
    auto* tests_end   = const_cast<test_entry*>(__stlx_unit_test_end);
    const size_t total_tests = static_cast<size_t>(tests_end - tests_begin);

    if (total_tests == 0) {
        log::raw("KTAP version 1");
        log::raw("1..0");
        log::raw("STLX_TESTS_COMPLETE 0");
        return 0;
    }

    // Sort entries by suite name within each tier.
    // The linker already groups by tier (0 before 1 before 2, etc.),
    // but within a tier, entries from different TUs may be interleaved.
    // We sort the entire array by suite_name; since entries within the
    // same tier already share the same linker-section prefix ordering,
    // the sort groups tests by suite while preserving tier order for
    // suites in different tiers (different suite names sort differently).
    sort_by_suite(tests_begin, tests_end);

    // Count unique suites
    size_t suite_count = 1;
    for (size_t i = 1; i < total_tests; i++) {
        if (string::strcmp(tests_begin[i].suite_name,
                          tests_begin[i - 1].suite_name) != 0) {
            suite_count++;
        }
    }

    log::raw("KTAP version 1");
    log::raw("1..%zu", suite_count);

    uint32_t total_passed = 0;
    uint32_t total_failed = 0;
    uint32_t suite_number = 1;

    size_t i = 0;
    while (i < total_tests) {
        const char* suite_name = tests_begin[i].suite_name;

        // Count tests in this suite
        size_t suite_start = i;
        size_t suite_test_count = 0;
        while (i < total_tests &&
               string::strcmp(tests_begin[i].suite_name, suite_name) == 0) {
            suite_test_count++;
            i++;
        }

        // Suite KTAP header
        log::raw("  KTAP version 1");
        log::raw("  # Suite: %s", suite_name);
        log::raw("  1..%zu", suite_test_count);

        // Run before_all hook
        hook_fn before_all = find_hook(suite_name, hook_type::before_all);
        if (before_all && before_all() != 0) {
            for (size_t t = 0; t < suite_test_count; t++) {
                log::raw("  ok %zu %s # SKIP before_all failed",
                         t + 1, tests_begin[suite_start + t].test_name);
            }
            log::raw("ok %u %s # SKIP before_all failed", suite_number, suite_name);
            suite_number++;
            continue;
        }

        // Run tests
        hook_fn before_each = find_hook(suite_name, hook_type::before_each);
        hook_fn after_each  = find_hook(suite_name, hook_type::after_each);
        bool suite_failed = false;

        for (size_t t = 0; t < suite_test_count; t++) {
            auto& test = tests_begin[suite_start + t];

            // before_each
            if (before_each && before_each() != 0) {
                log::raw("  ok %zu %s # SKIP before_each failed",
                         t + 1, test.test_name);
                continue;
            }

            // Run test
            detail::current_failures = 0;
            test.fn();

            if (detail::current_failures == 0) {
                log::raw("  ok %zu %s", t + 1, test.test_name);
                total_passed++;
            } else {
                log::raw("  not ok %zu %s", t + 1, test.test_name);
                total_failed++;
                suite_failed = true;
            }

            // after_each
            if (after_each) {
                after_each();
            }
        }

        // Run after_all hook
        hook_fn after_all = find_hook(suite_name, hook_type::after_all);
        if (after_all) {
            after_all();
        }

        // Suite result
        if (suite_failed) {
            log::raw("not ok %u %s", suite_number, suite_name);
        } else {
            log::raw("ok %u %s", suite_number, suite_name);
        }

        suite_number++;
    }

    log::raw("# passed: %u, failed: %u", total_passed, total_failed);
    log::raw("STLX_TESTS_COMPLETE %d", total_failed > 0 ? 1 : 0);

    return static_cast<int32_t>(total_failed);
}

} // namespace stlx_test
