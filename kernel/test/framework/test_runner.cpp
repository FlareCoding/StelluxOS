#include "test/framework/test_runner.h"
#include "common/logging.h"

#ifndef UNIT_TEST_FILTER
#define UNIT_TEST_FILTER ""
#endif

#ifndef UNIT_TEST_FAIL_FAST
#define UNIT_TEST_FAIL_FAST 0
#endif

#ifndef UNIT_TEST_REPEAT
#define UNIT_TEST_REPEAT 1
#endif

#ifndef UNIT_TEST_SEED
#define UNIT_TEST_SEED 0xC0FFEE
#endif

extern "C" {
extern const test::suite_desc __stlx_test_suites_start[];
extern const test::suite_desc __stlx_test_suites_end[];
extern const test::case_desc __stlx_test_cases_start[];
extern const test::case_desc __stlx_test_cases_end[];
}

namespace test {

static summary g_summary = {};
static bool g_registry_checked = false;
static bool g_registry_valid = false;
static bool g_any_failure = false;

static const char* k_filter = UNIT_TEST_FILTER;
static constexpr bool k_fail_fast = UNIT_TEST_FAIL_FAST != 0;
static constexpr uint64_t k_seed = static_cast<uint64_t>(UNIT_TEST_SEED);

static uint32_t repeat_count() {
    uint32_t repeat = static_cast<uint32_t>(UNIT_TEST_REPEAT);
    if (repeat == 0) {
        repeat = 1;
    }
    return repeat;
}

static size_t suite_count() {
    uintptr_t start = reinterpret_cast<uintptr_t>(__stlx_test_suites_start);
    uintptr_t end = reinterpret_cast<uintptr_t>(__stlx_test_suites_end);
    if (end <= start) {
        return 0;
    }
    return (end - start) / sizeof(suite_desc);
}

static size_t case_count() {
    uintptr_t start = reinterpret_cast<uintptr_t>(__stlx_test_cases_start);
    uintptr_t end = reinterpret_cast<uintptr_t>(__stlx_test_cases_end);
    if (end <= start) {
        return 0;
    }
    return (end - start) / sizeof(case_desc);
}

static bool str_empty(const char* s) {
    return s == nullptr || s[0] == '\0';
}

static bool str_eq(const char* a, const char* b) {
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a && *b) {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool str_eq_n(const char* a, const char* b, size_t n) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static bool str_starts_with(const char* str, const char* prefix) {
    if (str == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix) {
        if (*str == '\0' || *str != *prefix) {
            return false;
        }
        ++str;
        ++prefix;
    }
    return true;
}

static const char* find_char(const char* s, char c) {
    if (s == nullptr) {
        return nullptr;
    }
    while (*s) {
        if (*s == c) {
            return s;
        }
        ++s;
    }
    return nullptr;
}

static bool suite_known(const suite_desc* suite) {
    size_t suites = suite_count();
    for (size_t i = 0; i < suites; i++) {
        if (&__stlx_test_suites_start[i] == suite) {
            return true;
        }
    }
    return false;
}

static bool filter_match(const char* suite_name, const char* case_name) {
    if (str_empty(k_filter)) {
        return true;
    }

    const char* dot = find_char(k_filter, '.');
    if (dot == nullptr) {
        return str_starts_with(suite_name, k_filter);
    }

    size_t suite_len = static_cast<size_t>(dot - k_filter);
    if (!str_eq_n(suite_name, k_filter, suite_len) || suite_name[suite_len] != '\0') {
        return false;
    }

    const char* case_filter = dot + 1;
    if (case_filter[0] == '\0') {
        return true;
    }
    return str_eq(case_name, case_filter);
}

static const char* phase_name(phase p) {
    switch (p) {
        case phase::early: return "early";
        case phase::post_mm: return "post_mm";
        default: return "unknown";
    }
}

static uint64_t hash_cstr(const char* s, uint64_t seed) {
    uint64_t h = seed ^ 0xcbf29ce484222325ULL;
    if (s == nullptr) {
        return h;
    }
    while (*s) {
        h ^= static_cast<uint8_t>(*s);
        h *= 0x100000001b3ULL;
        ++s;
    }
    return h;
}

static uint64_t derive_seed(const char* suite_name, const char* case_name, uint32_t iteration) {
    uint64_t h = hash_cstr(suite_name, k_seed);
    h = hash_cstr(case_name, h);
    h ^= static_cast<uint64_t>(iteration) * 0x9e3779b97f4a7c15ULL;
    if (h == 0) {
        h = 0x9e3779b97f4a7c15ULL;
    }
    return h;
}

static int32_t validate_registry() {
    size_t suites = suite_count();
    size_t cases = case_count();

    for (size_t i = 0; i < suites; i++) {
        const suite_desc& suite = __stlx_test_suites_start[i];

        if (suite.abi_version != ABI_VERSION) {
            log::error("test registry: suite '%s' ABI mismatch (%u != %u)",
                       suite.name ? suite.name : "(null)",
                       suite.abi_version,
                       ABI_VERSION);
            return ERR_INVALID_REGISTRY;
        }
        if (str_empty(suite.name)) {
            log::error("test registry: suite[%lu] has empty name", i);
            return ERR_INVALID_REGISTRY;
        }
    }

    for (size_t i = 0; i < suites; i++) {
        const suite_desc& a = __stlx_test_suites_start[i];
        for (size_t j = i + 1; j < suites; j++) {
            const suite_desc& b = __stlx_test_suites_start[j];
            if (str_eq(a.name, b.name)) {
                log::error("test registry: duplicate suite name '%s'", a.name);
                return ERR_INVALID_REGISTRY;
            }
        }
    }

    for (size_t i = 0; i < cases; i++) {
        const case_desc& test_case = __stlx_test_cases_start[i];

        if (test_case.abi_version != ABI_VERSION) {
            log::error("test registry: case '%s' ABI mismatch (%u != %u)",
                       test_case.name ? test_case.name : "(null)",
                       test_case.abi_version,
                       ABI_VERSION);
            return ERR_INVALID_REGISTRY;
        }
        if (!suite_known(test_case.suite)) {
            log::error("test registry: case '%s' references unknown suite",
                       test_case.name ? test_case.name : "(null)");
            return ERR_INVALID_REGISTRY;
        }
        if (str_empty(test_case.name)) {
            log::error("test registry: case[%lu] has empty name", i);
            return ERR_INVALID_REGISTRY;
        }
        if (test_case.body == nullptr) {
            log::error("test registry: case '%s' has null body", test_case.name);
            return ERR_INVALID_REGISTRY;
        }
    }

    for (size_t i = 0; i < cases; i++) {
        const case_desc& a = __stlx_test_cases_start[i];
        for (size_t j = i + 1; j < cases; j++) {
            const case_desc& b = __stlx_test_cases_start[j];
            if (a.suite == b.suite && str_eq(a.name, b.name)) {
                log::error("test registry: duplicate case name '%s.%s'",
                           a.suite ? a.suite->name : "(null)",
                           a.name);
                return ERR_INVALID_REGISTRY;
            }
        }
    }

    if (cases == 0) {
        log::warn("test registry: no test cases registered");
    }

    return OK;
}

static bool suite_has_matching_cases(const suite_desc* suite, phase run_phase) {
    size_t cases = case_count();
    for (size_t i = 0; i < cases; i++) {
        const case_desc& test_case = __stlx_test_cases_start[i];
        if (test_case.suite != suite) {
            continue;
        }
        if (suite->run_phase != run_phase) {
            continue;
        }
        if (!filter_match(suite->name, test_case.name)) {
            continue;
        }
        return true;
    }
    return false;
}

static void maybe_validate_registry() {
    if (g_registry_checked) {
        return;
    }
    g_registry_valid = (validate_registry() == OK);
    g_registry_checked = true;
}

int32_t run_phase(phase run_phase) {
    maybe_validate_registry();
    if (!g_registry_valid) {
        return ERR_INVALID_REGISTRY;
    }

    size_t suites = suite_count();
    size_t cases = case_count();
    uint32_t repeats = repeat_count();

    uint32_t phase_suite_start = g_summary.suites_executed;
    uint32_t phase_case_start = g_summary.cases_executed;
    uint32_t phase_pass_start = g_summary.cases_passed;
    uint32_t phase_fail_start = g_summary.cases_failed;
    uint32_t phase_skip_start = g_summary.cases_skipped;

    log::info("[TEST_PHASE_BEGIN] %s", phase_name(run_phase));

    for (size_t si = 0; si < suites; si++) {
        const suite_desc& suite = __stlx_test_suites_start[si];
        if (suite.run_phase != run_phase) {
            continue;
        }
        if (!suite_has_matching_cases(&suite, run_phase)) {
            continue;
        }

        g_summary.suites_executed++;
        log::info("[TEST_SUITE_BEGIN] %s", suite.name);

        for (size_t ci = 0; ci < cases; ci++) {
            const case_desc& test_case = __stlx_test_cases_start[ci];
            if (test_case.suite != &suite) {
                continue;
            }
            if (!filter_match(suite.name, test_case.name)) {
                continue;
            }

            for (uint32_t iter = 0; iter < repeats; iter++) {
                context ctx = {
                    .suite_name = suite.name,
                    .case_name = test_case.name,
                    .seed = derive_seed(suite.name, test_case.name, iter),
                    .prng_state = derive_seed(suite.name, test_case.name, iter),
                    .iteration = iter,
                    .expectation_failures = 0,
                    .aborted = false,
                    .skipped = false,
                    .skip_reason = nullptr,
                };

                if (suite.before_each) {
                    suite.before_each(ctx);
                }

                test_case.body(ctx);

                if (suite.after_each) {
                    suite.after_each(ctx);
                }

                g_summary.cases_executed++;
                g_summary.expectation_failures += ctx.expectation_failures;

                if (ctx.skipped) {
                    g_summary.cases_skipped++;
                    log::warn("[TEST_CASE_SKIP] %s.%s[%u] reason=%s",
                              suite.name,
                              test_case.name,
                              iter,
                              ctx.skip_reason ? ctx.skip_reason : "(no reason)");
                    continue;
                }

                if (ctx.aborted || ctx.expectation_failures > 0) {
                    g_summary.cases_failed++;
                    g_any_failure = true;
                    log::error("[TEST_CASE_FAIL] %s.%s[%u] expectations=%u aborted=%u seed=0x%lx",
                               suite.name,
                               test_case.name,
                               iter,
                               ctx.expectation_failures,
                               ctx.aborted ? 1 : 0,
                               ctx.seed);

                    if (k_fail_fast) {
                        log::error("[TEST_PHASE_ABORT] %s fail-fast triggered", phase_name(run_phase));
                        log::info("[TEST_PHASE_END] %s suites=%u cases=%u passed=%u failed=%u skipped=%u",
                                  phase_name(run_phase),
                                  g_summary.suites_executed - phase_suite_start,
                                  g_summary.cases_executed - phase_case_start,
                                  g_summary.cases_passed - phase_pass_start,
                                  g_summary.cases_failed - phase_fail_start,
                                  g_summary.cases_skipped - phase_skip_start);
                        return ERR_FAILED;
                    }
                } else {
                    g_summary.cases_passed++;
                    log::info("[TEST_CASE_PASS] %s.%s[%u]",
                              suite.name,
                              test_case.name,
                              iter);
                }
            }
        }
    }

    log::info("[TEST_PHASE_END] %s suites=%u cases=%u passed=%u failed=%u skipped=%u",
              phase_name(run_phase),
              g_summary.suites_executed - phase_suite_start,
              g_summary.cases_executed - phase_case_start,
              g_summary.cases_passed - phase_pass_start,
              g_summary.cases_failed - phase_fail_start,
              g_summary.cases_skipped - phase_skip_start);

    return OK;
}

void print_summary() {
    maybe_validate_registry();
    log::info("[TEST_SUMMARY] suites=%u cases=%u passed=%u failed=%u skipped=%u expectations=%u",
              g_summary.suites_executed,
              g_summary.cases_executed,
              g_summary.cases_passed,
              g_summary.cases_failed,
              g_summary.cases_skipped,
              g_summary.expectation_failures);
}

bool all_passed() {
    maybe_validate_registry();
    return g_registry_valid && !g_any_failure && g_summary.cases_failed == 0;
}

void fail_check(context& ctx, const char* file, uint32_t line, const char* check) {
    ctx.expectation_failures++;
    log::error("[TEST_EXPECT] %s.%s[%u] %s:%u check failed: %s",
               ctx.suite_name,
               ctx.case_name,
               ctx.iteration,
               file,
               line,
               check);
}

void fail_check_values(
    context& ctx,
    const char* file,
    uint32_t line,
    const char* lhs_expr,
    const char* rhs_expr,
    uint64_t lhs,
    uint64_t rhs
) {
    ctx.expectation_failures++;
    log::error("[TEST_EXPECT] %s.%s[%u] %s:%u expected %s == %s (lhs=0x%lx rhs=0x%lx)",
               ctx.suite_name,
               ctx.case_name,
               ctx.iteration,
               file,
               line,
               lhs_expr,
               rhs_expr,
               lhs,
               rhs);
}

void fail_check_strings(
    context& ctx,
    const char* file,
    uint32_t line,
    const char* lhs_expr,
    const char* rhs_expr,
    const char* lhs,
    const char* rhs
) {
    ctx.expectation_failures++;
    log::error("[TEST_EXPECT] %s.%s[%u] %s:%u expected %s == %s (lhs=\"%s\" rhs=\"%s\")",
               ctx.suite_name,
               ctx.case_name,
               ctx.iteration,
               file,
               line,
               lhs_expr,
               rhs_expr,
               lhs ? lhs : "(null)",
               rhs ? rhs : "(null)");
}

void fail_message(context& ctx, const char* file, uint32_t line, const char* message) {
    ctx.expectation_failures++;
    log::error("[TEST_EXPECT] %s.%s[%u] %s:%u %s",
               ctx.suite_name,
               ctx.case_name,
               ctx.iteration,
               file,
               line,
               message ? message : "(null)");
}

void abort_case(context& ctx) {
    ctx.aborted = true;
}

void skip_case(context& ctx, const char* reason) {
    ctx.skipped = true;
    ctx.skip_reason = reason;
}

uint64_t random_next_u64(context& ctx) {
    uint64_t x = ctx.prng_state;
    if (x == 0) {
        x = 0x9e3779b97f4a7c15ULL;
    }

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;

    ctx.prng_state = x;
    return x * 0x2545f4914f6cdd1dULL;
}

uint64_t context_seed(const context& ctx) {
    return ctx.seed;
}

} // namespace test
