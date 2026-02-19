#ifndef STELLUX_TEST_FRAMEWORK_TEST_FRAMEWORK_H
#define STELLUX_TEST_FRAMEWORK_TEST_FRAMEWORK_H

#include "test_runner.h"
#include "common/types.h"

namespace test {

inline bool cstr_equal(const char* a, const char* b) {
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

template<typename T>
inline uint64_t to_u64(const T& value) {
    if constexpr (__is_pointer(T)) {
        return reinterpret_cast<uint64_t>(value);
    } else {
        return static_cast<uint64_t>(value);
    }
}

inline uint64_t to_u64(decltype(nullptr)) {
    return 0;
}

inline bool expect_true(
    context& ctx,
    bool cond,
    const char* expr,
    const char* file,
    uint32_t line
) {
    if (cond) {
        return true;
    }
    fail_check(ctx, file, line, expr);
    return false;
}

inline bool expect_false(
    context& ctx,
    bool cond,
    const char* expr,
    const char* file,
    uint32_t line
) {
    if (!cond) {
        return true;
    }
    fail_message(ctx, file, line, expr);
    return false;
}

template<typename L, typename R>
inline bool expect_eq(
    context& ctx,
    const L& lhs,
    const R& rhs,
    const char* lhs_expr,
    const char* rhs_expr,
    const char* file,
    uint32_t line
) {
    if (lhs == rhs) {
        return true;
    }
    fail_check_values(ctx, file, line, lhs_expr, rhs_expr, to_u64(lhs), to_u64(rhs));
    return false;
}

template<typename L, typename R>
inline bool expect_ne(
    context& ctx,
    const L& lhs,
    const R& rhs,
    const char* lhs_expr,
    const char* rhs_expr,
    const char* file,
    uint32_t line
) {
    if (lhs != rhs) {
        return true;
    }
    fail_message(ctx, file, line, "expected values to differ");
    fail_check_values(ctx, file, line, lhs_expr, rhs_expr, to_u64(lhs), to_u64(rhs));
    return false;
}

inline bool expect_streq(
    context& ctx,
    const char* lhs,
    const char* rhs,
    const char* lhs_expr,
    const char* rhs_expr,
    const char* file,
    uint32_t line
) {
    if (cstr_equal(lhs, rhs)) {
        return true;
    }
    fail_check_strings(ctx, file, line, lhs_expr, rhs_expr, lhs, rhs);
    return false;
}

} // namespace test

#define STLX_TEST_SUITE_EX(suite_ident, suite_phase, before_hook, after_hook) \
    namespace { \
    __attribute__((used, section(".stlx_test_suites." #suite_ident))) \
    const ::test::suite_desc stlx_test_suite_##suite_ident = { \
        .abi_version = ::test::ABI_VERSION, \
        .name = #suite_ident, \
        .run_phase = (suite_phase), \
        .before_each = (before_hook), \
        .after_each = (after_hook), \
    }; \
    }

#define STLX_TEST_SUITE(suite_ident, suite_phase) \
    STLX_TEST_SUITE_EX(suite_ident, suite_phase, nullptr, nullptr)

#define STLX_TEST(suite_ident, case_ident) \
    static __PRIVILEGED_CODE void stlx_test_case_##suite_ident##_##case_ident(::test::context& ctx); \
    namespace { \
    __attribute__((used, section(".stlx_test_cases." #suite_ident "." #case_ident))) \
    const ::test::case_desc stlx_test_case_desc_##suite_ident##_##case_ident = { \
        .abi_version = ::test::ABI_VERSION, \
        .suite = &stlx_test_suite_##suite_ident, \
        .name = #case_ident, \
        .body = stlx_test_case_##suite_ident##_##case_ident, \
    }; \
    } \
    static __PRIVILEGED_CODE void stlx_test_case_##suite_ident##_##case_ident(::test::context& ctx)

#define STLX_EXPECT_TRUE(ctx, expr) \
    ::test::expect_true((ctx), static_cast<bool>(expr), #expr, __FILE__, static_cast<uint32_t>(__LINE__))

#define STLX_EXPECT_FALSE(ctx, expr) \
    ::test::expect_false((ctx), static_cast<bool>(expr), #expr, __FILE__, static_cast<uint32_t>(__LINE__))

#define STLX_EXPECT_EQ(ctx, lhs, rhs) \
    ::test::expect_eq((ctx), (lhs), (rhs), #lhs, #rhs, __FILE__, static_cast<uint32_t>(__LINE__))

#define STLX_EXPECT_NE(ctx, lhs, rhs) \
    ::test::expect_ne((ctx), (lhs), (rhs), #lhs, #rhs, __FILE__, static_cast<uint32_t>(__LINE__))

#define STLX_EXPECT_NULL(ctx, value) \
    STLX_EXPECT_EQ((ctx), (value), nullptr)

#define STLX_EXPECT_NOT_NULL(ctx, value) \
    STLX_EXPECT_NE((ctx), (value), nullptr)

#define STLX_EXPECT_STREQ(ctx, lhs, rhs) \
    ::test::expect_streq((ctx), (lhs), (rhs), #lhs, #rhs, __FILE__, static_cast<uint32_t>(__LINE__))

#define STLX_ASSERT_TRUE(ctx, expr) \
    do { \
        if (!STLX_EXPECT_TRUE((ctx), (expr))) { \
            ::test::abort_case((ctx)); \
            return; \
        } \
    } while (0)

#define STLX_ASSERT_FALSE(ctx, expr) \
    do { \
        if (!STLX_EXPECT_FALSE((ctx), (expr))) { \
            ::test::abort_case((ctx)); \
            return; \
        } \
    } while (0)

#define STLX_ASSERT_EQ(ctx, lhs, rhs) \
    do { \
        if (!STLX_EXPECT_EQ((ctx), (lhs), (rhs))) { \
            ::test::abort_case((ctx)); \
            return; \
        } \
    } while (0)

#define STLX_ASSERT_NE(ctx, lhs, rhs) \
    do { \
        if (!STLX_EXPECT_NE((ctx), (lhs), (rhs))) { \
            ::test::abort_case((ctx)); \
            return; \
        } \
    } while (0)

#define STLX_ASSERT_NULL(ctx, value) \
    do { \
        if (!STLX_EXPECT_NULL((ctx), (value))) { \
            ::test::abort_case((ctx)); \
            return; \
        } \
    } while (0)

#define STLX_ASSERT_NOT_NULL(ctx, value) \
    do { \
        if (!STLX_EXPECT_NOT_NULL((ctx), (value))) { \
            ::test::abort_case((ctx)); \
            return; \
        } \
    } while (0)

#define STLX_ASSERT_STREQ(ctx, lhs, rhs) \
    do { \
        if (!STLX_EXPECT_STREQ((ctx), (lhs), (rhs))) { \
            ::test::abort_case((ctx)); \
            return; \
        } \
    } while (0)

#define STLX_FAIL(ctx, message) \
    do { \
        ::test::fail_message((ctx), __FILE__, static_cast<uint32_t>(__LINE__), (message)); \
        ::test::abort_case((ctx)); \
        return; \
    } while (0)

#define STLX_SKIP(ctx, reason) \
    do { \
        ::test::skip_case((ctx), (reason)); \
        return; \
    } while (0)

#endif // STELLUX_TEST_FRAMEWORK_TEST_FRAMEWORK_H
