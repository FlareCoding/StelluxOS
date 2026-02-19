#ifndef STELLUX_TEST_FRAMEWORK_TEST_RUNNER_H
#define STELLUX_TEST_FRAMEWORK_TEST_RUNNER_H

#include "test_registry.h"
#include "common/types.h"

namespace test {

constexpr int32_t OK                   = 0;
constexpr int32_t ERR_FAILED           = -1;
constexpr int32_t ERR_INVALID_REGISTRY = -2;

struct context {
    const char* suite_name;
    const char* case_name;
    uint64_t seed;
    uint64_t prng_state;
    uint32_t iteration;
    uint32_t expectation_failures;
    bool aborted;
    bool skipped;
    const char* skip_reason;
};

struct summary {
    uint32_t suites_executed;
    uint32_t cases_executed;
    uint32_t cases_passed;
    uint32_t cases_failed;
    uint32_t cases_skipped;
    uint32_t expectation_failures;
};

int32_t run_phase(phase run_phase);
void print_summary();
bool all_passed();

void fail_check(context& ctx, const char* file, uint32_t line, const char* check);
void fail_check_values(
    context& ctx,
    const char* file,
    uint32_t line,
    const char* lhs_expr,
    const char* rhs_expr,
    uint64_t lhs,
    uint64_t rhs
);
void fail_check_strings(
    context& ctx,
    const char* file,
    uint32_t line,
    const char* lhs_expr,
    const char* rhs_expr,
    const char* lhs,
    const char* rhs
);
void fail_message(context& ctx, const char* file, uint32_t line, const char* message);

void abort_case(context& ctx);
void skip_case(context& ctx, const char* reason);

uint64_t random_next_u64(context& ctx);
uint64_t context_seed(const context& ctx);

} // namespace test

#endif // STELLUX_TEST_FRAMEWORK_TEST_RUNNER_H
