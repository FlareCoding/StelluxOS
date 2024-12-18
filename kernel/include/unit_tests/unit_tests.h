#ifdef BUILD_UNIT_TESTS
#ifndef UNIT_TEST_H
#define UNIT_TEST_H
#include <types.h>
#include <serial/serial.h>

// Define return values for test outcomes
#define UNIT_TEST_SUCCESS          0
#define UNIT_TEST_FAILURE          1
#define UNIT_TEST_CRITICAL_FAILURE 2

// Unit test printing prefix
#define UNIT_TEST_PREFIX "[TEST] "

// Define a type for the test function
typedef int (*test_func_t)();

// Macro for registering a test with a name and adding it to the section
#define DECLARE_UNIT_TEST(test_name, test_func) \
    int test_func(); \
    static const unit_test_t __attribute__((used, section(".unit_test"))) test_struct_##test_func = { #test_name, test_func }; \
    int test_func()

#define DECLARE_UNUSED_UNIT_TEST(test_name, test_func) \
    int test_func(); \
    static const unit_test_t __attribute__((unused, section(".unit_test"))) test_struct_##test_func = { #test_name, test_func }; \
    int test_func()

// Structure to hold test information (name and function pointer)
struct unit_test_t {
    const char* name;
    test_func_t func;
};

// This will iterate over all unit tests at runtime
void execute_unit_tests();

// Macro for soft assertion failure, continues execution of other unit tests
#define ASSERT_EQ(value, expected, fmt, ...) \
    do { \
        if ((value) != (expected)) { \
            serial::printf("[ASSERT] %s:%i, ", __FILE__, __LINE__); \
            serial::printf(fmt, ##__VA_ARGS__); \
            serial::printf(", expected %lli but got %lli\n", (uint64_t)(expected), (uint64_t)(value)); \
            return UNIT_TEST_FAILURE; \
        } \
    } while (0)

// Macro for critical assertion failure, triggers a shutdown
#define ASSERT_EQ_CRITICAL(value, expected, fmt, ...) \
    do { \
        if ((value) != (expected)) { \
            serial::printf("[ASSERT] %s:%i, ", __FILE__, __LINE__); \
            serial::printf(fmt, ##__VA_ARGS__); \
            serial::printf(", expected %lli but got %lli\n", (uint64_t)(expected), (uint64_t)(value)); \
            serial::printf("[ASSERT] Critical failure detected, shutting down.\n"); \
            return UNIT_TEST_CRITICAL_FAILURE; \
        } \
    } while (0)

// Macro for soft assertion failure of strings, continues execution of other unit tests
#define ASSERT_STR_EQ(value, expected, fmt, ...) \
    do { \
        if (strcmp((value), (expected)) != 0) { \
            serial::printf("[ASSERT] %s:%i, ", __FILE__, __LINE__); \
            serial::printf(fmt, ##__VA_ARGS__); \
            serial::printf(", expected '%s' but got '%s'\n", (expected), (value)); \
            return UNIT_TEST_FAILURE; \
        } \
    } while (0)

// Macro for critical assertion failure of strings, triggers a shutdown
#define ASSERT_STR_EQ_CRITICAL(value, expected, fmt, ...) \
    do { \
        if (strcmp((value), (expected)) != 0) { \
            serial::printf("[ASSERT] %s:%i, ", __FILE__, __LINE__); \
            serial::printf(fmt, ##__VA_ARGS__); \
            serial::printf(", expected '%s' but got '%s'\n", (expected), (value)); \
            serial::printf("[ASSERT] Critical failure detected, shutting down.\n"); \
            return UNIT_TEST_CRITICAL_FAILURE; \
        } \
    } while (0)

// Macro for general condition assertion, continues execution of other unit tests on failure
#define ASSERT_TRUE(condition, fmt, ...) \
    do { \
        if (!(condition)) { \
            serial::printf("[ASSERT] %s:%i, ", __FILE__, __LINE__); \
            serial::printf(fmt, ##__VA_ARGS__); \
            serial::printf(", condition failed\n"); \
            return UNIT_TEST_FAILURE; \
        } \
    } while (0)

#define ASSERT_FALSE(condition, fmt, ...) \
    do { \
        if ((condition)) { \
            serial::printf("[ASSERT] %s:%i, ", __FILE__, __LINE__); \
            serial::printf(fmt, ##__VA_ARGS__); \
            serial::printf(", condition failed\n"); \
            return UNIT_TEST_FAILURE; \
        } \
    } while (0)

// Macro for critical condition assertion, triggers a shutdown on failure
#define ASSERT_TRUE_CRITICAL(condition, fmt, ...) \
    do { \
        if (!(condition)) { \
            serial::printf("[ASSERT] %s:%i, ", __FILE__, __LINE__); \
            serial::printf(fmt, ##__VA_ARGS__); \
            serial::printf(", condition failed\n"); \
            serial::printf("[ASSERT] Critical failure detected, shutting down.\n"); \
            return UNIT_TEST_CRITICAL_FAILURE; \
        } \
    } while (0)

#endif // UNIT_TEST_H
#endif // BUILD_UNIT_TESTS
