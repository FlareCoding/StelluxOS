#ifndef STELLUX_TESTS_FRAMEWORK_STLX_UNIT_TEST_H
#define STELLUX_TESTS_FRAMEWORK_STLX_UNIT_TEST_H

#include "common/types.h"
#include "common/string.h"

// Tier constants -- #define macros because they must be stringified
// by the preprocessor for linker section names
#define TIER_UTIL     0
#define TIER_DS       1
#define TIER_MM_CORE  2
#define TIER_MM_ALLOC 3
#define TIER_SCHED    4

namespace stlx_test {

struct test_entry {
    const char* suite_name;
    const char* test_name;
    void (*fn)();
};

static_assert(sizeof(test_entry) % 8 == 0,
    "test_entry size must be a multiple of 8 for linker section alignment");

enum class hook_type : uint8_t {
    before_all  = 0,
    after_all   = 1,
    before_each = 2,
    after_each  = 3,
};

using hook_fn = int32_t (*)();

struct suite_hooks {
    const char* suite_name;
    hook_fn fn;
    hook_type type;
    uint8_t _pad[7];
};

static_assert(sizeof(suite_hooks) % 8 == 0,
    "suite_hooks size must be a multiple of 8 for linker section alignment");

namespace detail {

inline uint32_t current_failures = 0;

void report_fail(const char* file, int line, const char* macro, const char* expr);

template<typename T>
void format_value(const char* label, const T& val) {
    // Forward declaration -- implemented in runner.cpp via log::raw
    // We call print() which is declared in runner.h, but to avoid
    // circular includes we use a non-template helper for output.
    // This is handled via the overloaded report functions below.
    (void)label;
    (void)val;
}

void report_values_uint(const char* a_str, const char* b_str, uint64_t lhs, uint64_t rhs);
void report_values_ptr(const char* a_str, const char* b_str, uintptr_t lhs, uintptr_t rhs);
void report_values_bool(const char* a_str, const char* b_str, bool lhs, bool rhs);
void report_values_str(const char* a_str, const char* b_str, const char* lhs, const char* rhs);

template<typename T, typename U>
void report_eq_failure(const char* file, int line,
                       const char* a_str, const char* b_str,
                       const T& lhs, const U& rhs) {
    report_fail(file, line, "EXPECT_EQ", nullptr);
    if constexpr (__is_same(T, bool) && __is_same(U, bool)) {
        report_values_bool(a_str, b_str, lhs, rhs);
    } else if constexpr (__is_pointer(T) || __is_pointer(U) ||
                         __is_same(T, decltype(nullptr)) || __is_same(U, decltype(nullptr))) {
        report_values_ptr(a_str, b_str,
            reinterpret_cast<uintptr_t>(lhs), reinterpret_cast<uintptr_t>(rhs));
    } else {
        report_values_uint(a_str, b_str,
            static_cast<uint64_t>(lhs), static_cast<uint64_t>(rhs));
    }
}

template<typename T, typename U>
void report_ne_failure(const char* file, int line,
                       const char* a_str, const char* b_str,
                       const T& lhs, const U& rhs) {
    report_fail(file, line, "EXPECT_NE", nullptr);
    if constexpr (__is_pointer(T) || __is_pointer(U)) {
        report_values_ptr(a_str, b_str,
            reinterpret_cast<uintptr_t>(lhs), reinterpret_cast<uintptr_t>(rhs));
    } else {
        report_values_uint(a_str, b_str,
            static_cast<uint64_t>(lhs), static_cast<uint64_t>(rhs));
    }
}

template<typename T, typename U>
void report_cmp_failure(const char* file, int line, const char* macro,
                        const char* a_str, const char* b_str,
                        const T& lhs, const U& rhs) {
    report_fail(file, line, macro, nullptr);
    report_values_uint(a_str, b_str,
        static_cast<uint64_t>(lhs), static_cast<uint64_t>(rhs));
}

} // namespace detail
} // namespace stlx_test

// Stringify helpers
#define STLX_STRINGIFY_(x) #x
#define STLX_STRINGIFY(x) STLX_STRINGIFY_(x)
#define STLX_CONCAT_(a, b) a##b
#define STLX_CONCAT(a, b) STLX_CONCAT_(a, b)

// Define STLX_TEST_TIER before including this header to set the tier
#ifndef STLX_TEST_TIER
#define STLX_TEST_TIER TIER_UTIL
#endif

// Registration macros
#define TEST_SUITE(name) \
    [[maybe_unused]] static constexpr const char* stlx_suite_##name = #name

#define TEST(suite, name) \
    static void stlx_test_##suite##_##name(); \
    __attribute__((used, section(".stlx_unit_test." STLX_STRINGIFY(STLX_TEST_TIER) "." #suite))) \
    static const ::stlx_test::test_entry stlx_entry_##suite##_##name = { \
        #suite, #name, stlx_test_##suite##_##name \
    }; \
    static void stlx_test_##suite##_##name()

// Hook macros
#define BEFORE_ALL(suite, fn) \
    __attribute__((used, section(".stlx_unit_test_hooks"))) \
    static const ::stlx_test::suite_hooks stlx_hook_##suite##_before_all = { \
        #suite, fn, ::stlx_test::hook_type::before_all, {} \
    }

#define AFTER_ALL(suite, fn) \
    __attribute__((used, section(".stlx_unit_test_hooks"))) \
    static const ::stlx_test::suite_hooks stlx_hook_##suite##_after_all = { \
        #suite, fn, ::stlx_test::hook_type::after_all, {} \
    }

#define BEFORE_EACH(suite, fn) \
    __attribute__((used, section(".stlx_unit_test_hooks"))) \
    static const ::stlx_test::suite_hooks stlx_hook_##suite##_before_each = { \
        #suite, fn, ::stlx_test::hook_type::before_each, {} \
    }

#define AFTER_EACH(suite, fn) \
    __attribute__((used, section(".stlx_unit_test_hooks"))) \
    static const ::stlx_test::suite_hooks stlx_hook_##suite##_after_each = { \
        #suite, fn, ::stlx_test::hook_type::after_each, {} \
    }

// Assertion macros

// Unique variable name per line to avoid shadowing
#define STLX_LHS STLX_CONCAT(stlx_lhs_, __LINE__)
#define STLX_RHS STLX_CONCAT(stlx_rhs_, __LINE__)
#define STLX_VAL STLX_CONCAT(stlx_val_, __LINE__)

// Equality

#define EXPECT_EQ(a, b) do { \
    const auto& STLX_LHS = (a); \
    const auto& STLX_RHS = (b); \
    if (!(STLX_LHS == STLX_RHS)) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_eq_failure( \
            __FILE__, __LINE__, #a, #b, STLX_LHS, STLX_RHS); \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    const auto& STLX_LHS = (a); \
    const auto& STLX_RHS = (b); \
    if (!(STLX_LHS == STLX_RHS)) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_eq_failure( \
            __FILE__, __LINE__, #a, #b, STLX_LHS, STLX_RHS); \
        return; \
    } \
} while (0)

#define EXPECT_NE(a, b) do { \
    const auto& STLX_LHS = (a); \
    const auto& STLX_RHS = (b); \
    if (STLX_LHS == STLX_RHS) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_ne_failure( \
            __FILE__, __LINE__, #a, #b, STLX_LHS, STLX_RHS); \
    } \
} while (0)

#define ASSERT_NE(a, b) do { \
    const auto& STLX_LHS = (a); \
    const auto& STLX_RHS = (b); \
    if (STLX_LHS == STLX_RHS) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_ne_failure( \
            __FILE__, __LINE__, #a, #b, STLX_LHS, STLX_RHS); \
        return; \
    } \
} while (0)

// Boolean

#define EXPECT_TRUE(expr) do { \
    const bool STLX_VAL = static_cast<bool>(expr); \
    if (!STLX_VAL) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "EXPECT_TRUE", #expr); \
    } \
} while (0)

#define ASSERT_TRUE(expr) do { \
    const bool STLX_VAL = static_cast<bool>(expr); \
    if (!STLX_VAL) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "ASSERT_TRUE", #expr); \
        return; \
    } \
} while (0)

#define EXPECT_FALSE(expr) do { \
    const bool STLX_VAL = static_cast<bool>(expr); \
    if (STLX_VAL) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "EXPECT_FALSE", #expr); \
    } \
} while (0)

#define ASSERT_FALSE(expr) do { \
    const bool STLX_VAL = static_cast<bool>(expr); \
    if (STLX_VAL) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "ASSERT_FALSE", #expr); \
        return; \
    } \
} while (0)

// Pointer

#define EXPECT_NULL(ptr) do { \
    const auto* STLX_VAL = (ptr); \
    if (STLX_VAL != nullptr) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "EXPECT_NULL", #ptr); \
    } \
} while (0)

#define ASSERT_NULL(ptr) do { \
    const auto* STLX_VAL = (ptr); \
    if (STLX_VAL != nullptr) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "ASSERT_NULL", #ptr); \
        return; \
    } \
} while (0)

#define EXPECT_NOT_NULL(ptr) do { \
    const auto* STLX_VAL = (ptr); \
    if (STLX_VAL == nullptr) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "EXPECT_NOT_NULL", #ptr); \
    } \
} while (0)

#define ASSERT_NOT_NULL(ptr) do { \
    const auto* STLX_VAL = (ptr); \
    if (STLX_VAL == nullptr) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "ASSERT_NOT_NULL", #ptr); \
        return; \
    } \
} while (0)

// Comparison

#define EXPECT_LT(a, b) do { \
    const auto& STLX_LHS = (a); \
    const auto& STLX_RHS = (b); \
    if (!(STLX_LHS < STLX_RHS)) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_cmp_failure( \
            __FILE__, __LINE__, "EXPECT_LT", #a, #b, STLX_LHS, STLX_RHS); \
    } \
} while (0)

#define EXPECT_LE(a, b) do { \
    const auto& STLX_LHS = (a); \
    const auto& STLX_RHS = (b); \
    if (!(STLX_LHS <= STLX_RHS)) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_cmp_failure( \
            __FILE__, __LINE__, "EXPECT_LE", #a, #b, STLX_LHS, STLX_RHS); \
    } \
} while (0)

#define EXPECT_GT(a, b) do { \
    const auto& STLX_LHS = (a); \
    const auto& STLX_RHS = (b); \
    if (!(STLX_LHS > STLX_RHS)) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_cmp_failure( \
            __FILE__, __LINE__, "EXPECT_GT", #a, #b, STLX_LHS, STLX_RHS); \
    } \
} while (0)

#define EXPECT_GE(a, b) do { \
    const auto& STLX_LHS = (a); \
    const auto& STLX_RHS = (b); \
    if (!(STLX_LHS >= STLX_RHS)) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_cmp_failure( \
            __FILE__, __LINE__, "EXPECT_GE", #a, #b, STLX_LHS, STLX_RHS); \
    } \
} while (0)

// Bitwise

#define EXPECT_BITS_SET(value, mask) do { \
    const auto& STLX_LHS = (value); \
    const auto& STLX_RHS = (mask); \
    if ((STLX_LHS & STLX_RHS) != STLX_RHS) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_cmp_failure( \
            __FILE__, __LINE__, "EXPECT_BITS_SET", #value, #mask, STLX_LHS, STLX_RHS); \
    } \
} while (0)

#define EXPECT_BITS_CLEAR(value, mask) do { \
    const auto& STLX_LHS = (value); \
    const auto& STLX_RHS = (mask); \
    if ((STLX_LHS & STLX_RHS) != 0) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_cmp_failure( \
            __FILE__, __LINE__, "EXPECT_BITS_CLEAR", #value, #mask, STLX_LHS, STLX_RHS); \
    } \
} while (0)

// Alignment

#define EXPECT_ALIGNED(ptr, alignment) do { \
    const uintptr_t stlx_addr_ = reinterpret_cast<uintptr_t>(ptr); \
    const uintptr_t stlx_align_ = (alignment); \
    if (stlx_align_ == 0 || (stlx_align_ & (stlx_align_ - 1)) != 0) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, \
            "EXPECT_ALIGNED", "alignment must be power of 2"); \
    } else if ((stlx_addr_ & (stlx_align_ - 1)) != 0) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "EXPECT_ALIGNED", #ptr); \
    } \
} while (0)

// String

#define EXPECT_STREQ(a, b) do { \
    const char* stlx_sa_ = (a); \
    const char* stlx_sb_ = (b); \
    if (string::strcmp(stlx_sa_, stlx_sb_) != 0) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "EXPECT_STREQ", nullptr); \
        ::stlx_test::detail::report_values_str(#a, #b, stlx_sa_, stlx_sb_); \
    } \
} while (0)

#define ASSERT_STREQ(a, b) do { \
    const char* stlx_sa_ = (a); \
    const char* stlx_sb_ = (b); \
    if (string::strcmp(stlx_sa_, stlx_sb_) != 0) { \
        ::stlx_test::detail::current_failures++; \
        ::stlx_test::detail::report_fail(__FILE__, __LINE__, "ASSERT_STREQ", nullptr); \
        ::stlx_test::detail::report_values_str(#a, #b, stlx_sa_, stlx_sb_); \
        return; \
    } \
} while (0)

#endif // STELLUX_TESTS_FRAMEWORK_STLX_UNIT_TEST_H
