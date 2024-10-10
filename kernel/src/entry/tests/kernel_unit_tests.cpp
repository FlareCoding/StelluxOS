#include "kernel_unit_tests.h"
#include <kelevate/kelevate.h>
#include <acpi/shutdown.h>

// Extern symbols provided by the linker that mark the start and end of the .unit_test section
extern KUnitTest __unit_tests_start[];
extern KUnitTest __unit_tests_end[];

uint64_t getUnitTestCount() {
    return ((uint64_t)__unit_tests_end - (uint64_t)__unit_tests_start) / sizeof(KUnitTest);
}

// Function to run all tests
void executeUnitTests() {
    uint64_t unitTestCount = ((uint64_t)__unit_tests_end - (uint64_t)__unit_tests_start) / sizeof(KUnitTest);
    size_t currentTestIndex;
    KUnitTest* test;
    int failures = 0;

    kprintf("\n=====================================\n");
    kprintf(UNIT_TEST "Starting Unit Tests\n");
    kprintf(UNIT_TEST "Total Tests: %lli\n", unitTestCount);
    kprintf("=====================================\n\n");

    for (currentTestIndex = 1, test = __unit_tests_start; test < __unit_tests_end; ++test, ++currentTestIndex) {
        kprintf("\n-------------------------------------\n");
        kprintf(UNIT_TEST "Test %lli of %lli\n", currentTestIndex, unitTestCount);
        kprintf(UNIT_TEST "Test Name: %s\n", test->name);
        kprintf("-------------------------------------\n");

        // Run the test and handle its return value
        int result = test->func();

        if (result == UNIT_TEST_SUCCESS) {
            kprintf(UNIT_TEST "Test %s passed!\n", test->name);
        } else if (result == UNIT_TEST_FAILURE) {
            kprintf(UNIT_TEST "Test %s failed, but continuing...\n", test->name);
            ++failures;
        } else if (result == UNIT_TEST_CRITICAL_FAILURE) {
            kprintf(UNIT_TEST "Test %s encountered a critical failure!\n", test->name);
            kprintf(UNIT_TEST "Critical failure detected, shutting down...\n");
            RUN_ELEVATED({
                vmshutdown();
            });
        }

        kprintf("-------------------------------------\n");
    }

    kprintf("\n=====================================\n");
    kprintf(UNIT_TEST "All Unit Tests Completed\n");
    kprintf(UNIT_TEST "Total Tests: %lli, Passed: %lli, Failed: %lli\n", unitTestCount, unitTestCount - failures, failures);
    kprintf("=====================================\n");
}
