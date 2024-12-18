#ifdef BUILD_UNIT_TESTS
#include <unit_tests/unit_tests.h>
#include <acpi/shutdown.h>

// Extern symbols provided by the linker that mark the start and end of the .unit_test section
extern unit_test_t __unit_tests_start[];
extern unit_test_t __unit_tests_end[];

uint64_t get_unit_test_count() {
    return ((uint64_t)__unit_tests_end - (uint64_t)__unit_tests_start) / sizeof(unit_test_t);
}

// Function to run all tests
void execute_unit_tests() {
    uint64_t unit_test_count = ((uint64_t)__unit_tests_end - (uint64_t)__unit_tests_start) / sizeof(unit_test_t);
    size_t current_test_index;
    unit_test_t* test;
    int failures = 0;

    serial::printf("\n=====================================\n");
    serial::printf(UNIT_TEST_PREFIX "Starting Unit Tests\n");
    serial::printf(UNIT_TEST_PREFIX "Total Tests: %lli\n", unit_test_count);
    serial::printf("=====================================\n\n");

    for (current_test_index = 1, test = __unit_tests_start; test < __unit_tests_end; ++test, ++current_test_index) {
        serial::printf("\n-------------------------------------\n");
        serial::printf(UNIT_TEST_PREFIX "Test %lli of %lli\n", current_test_index, unit_test_count);
        serial::printf(UNIT_TEST_PREFIX "Test Name: %s\n", test->name);
        serial::printf("-------------------------------------\n");

        // Run the test and handle its return value
        int result = test->func();

        if (result == UNIT_TEST_SUCCESS) {
            serial::printf(UNIT_TEST_PREFIX "Test %s passed!\n", test->name);
        } else if (result == UNIT_TEST_FAILURE) {
            serial::printf(UNIT_TEST_PREFIX "Test %s failed, but continuing...\n", test->name);
            ++failures;
        } else if (result == UNIT_TEST_CRITICAL_FAILURE) {
            serial::printf(UNIT_TEST_PREFIX "Test %s encountered a critical failure!\n", test->name);
            serial::printf(UNIT_TEST_PREFIX "Critical failure detected, shutting down...\n");
            vmshutdown();
        }

        serial::printf("-------------------------------------\n");
    }

    serial::printf("\n=====================================\n");
    serial::printf(UNIT_TEST_PREFIX "All Unit Tests Completed\n");
    serial::printf(UNIT_TEST_PREFIX "Total Tests: %lli, Passed: %lli, Failed: %lli\n", 
                   unit_test_count, unit_test_count - failures, failures);
    serial::printf("=====================================\n");
}

#endif // BUILD_UNIT_TESTS
