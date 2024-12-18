#include <unit_tests/unit_tests.h>

DECLARE_UNIT_TEST("Example Unit Test", example_unit_test) {
    ASSERT_TRUE(false, "This is an example unit test that should fail with error %i", 72);
    return UNIT_TEST_SUCCESS;
}
