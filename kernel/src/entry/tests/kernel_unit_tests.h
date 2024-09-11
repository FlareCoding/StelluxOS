#ifndef KERNEL_UNIT_TESTS_H
#define KERNEL_UNIT_TESTS_H
#include <ktypes.h>
#include <kprint.h>
#include <core/kstring.h>

// Define return values for test outcomes
#define UNIT_TEST_SUCCESS          0
#define UNIT_TEST_FAILURE          1
#define UNIT_TEST_CRITICAL_FAILURE 2

// Unit test printing prefix
#define UNIT_TEST "[TEST] "

// Define a type for the test function
typedef int (*testFunc_t)();

// Macro for registering a test with a name and adding it to the section
#define DECLARE_UNIT_TEST(testName, testFunc) \
    int testFunc(); \
    static const KUnitTest __UNIT_TEST testStruct_##testFunc = { #testName, testFunc }; \
    int testFunc()

// Structure to hold test information (name and function pointer)
struct KUnitTest {
    const char* name;
    testFunc_t func;
};

// This will iterate over all unit tests at runtime
void executeUnitTests();

// Macro for soft assertion failure, continues execution of other unit tests
#define ASSERT_EQ(value, expected, message) \
    do { \
        if ((value) != (expected)) { \
            kuPrint("[ASSERT] %s:%i, %s, expected %lli but got %lli\n", \
                __FILE__, __LINE__, message, (uint64_t)(expected), (uint64_t)(value)); \
            return UNIT_TEST_FAILURE; \
        } \
    } while (0)

// Macro for critical assertion failure, triggers a shutdown
#define ASSERT_EQ_CRITICAL(value, expected, message) \
    do { \
        if ((value) != (expected)) { \
            kuPrint("[ASSERT] %s:%i, %s, expected %lli but got %lli\n", \
                __FILE__, __LINE__, message, (uint64_t)(expected), (uint64_t)(value)); \
            kuPrint("[ASSERT] Critical failure detected, shutting down.\n"); \
            return UNIT_TEST_CRITICAL_FAILURE; \
        } \
    } while (0)

// Macro for soft assertion failure of strings, continues execution of other unit tests
#define ASSERT_STR_EQ(value, expected, message) \
    do { \
        if (strcmp((value), (expected)) != 0) { \
            kuPrint("[ASSERT] %s:%i, %s, expected %s but got %s\n", \
                __FILE__, __LINE__, message, (uint64_t)(expected), (uint64_t)(value)); \
            return UNIT_TEST_FAILURE; \
        } \
    } while (0)

// Macro for critical assertion failure of strings, triggers a shutdown
#define ASSERT_STR_EQ_CRITICAL(value, expected, message) \
    do { \
        if (strcmp((value), (expected)) != 0) { \
            kuPrint("[ASSERT] %s:%i, %s, expected %s but got %s\n", \
                __FILE__, __LINE__, message, (uint64_t)(expected), (uint64_t)(value)); \
            kuPrint("[ASSERT] Critical failure detected, shutting down.\n"); \
            return UNIT_TEST_CRITICAL_FAILURE; \
        } \
    } while (0)

// Macro for general condition assertion, continues execution of other unit tests on failure
#define ASSERT_TRUE(condition, message) \
    do { \
        if (!(condition)) { \
            kuPrint("[ASSERT] %s:%i, %s, condition failed\n", \
                __FILE__, __LINE__, message); \
            return UNIT_TEST_FAILURE; \
        } \
    } while (0)

// Macro for critical condition assertion, triggers a shutdown on failure
#define ASSERT_TRUE_CRITICAL(condition, message) \
    do { \
        if (!(condition)) { \
            kuPrint("[ASSERT] %s:%i, %s, condition failed\n", \
                __FILE__, __LINE__, message); \
            kuPrint("[ASSERT] Critical failure detected, shutting down.\n"); \
            return UNIT_TEST_CRITICAL_FAILURE; \
        } \
    } while (0)

#endif
