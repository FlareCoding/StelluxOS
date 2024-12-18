#include <unit_tests/unit_tests.h>

DECLARE_UNIT_TEST("Example Unit Test", example_unit_test) {
    ASSERT_TRUE(true, "Example unit test failed with error %i", 72);
    return UNIT_TEST_SUCCESS;
}
