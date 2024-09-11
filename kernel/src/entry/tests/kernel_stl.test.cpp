#include "kernel_unit_tests.h"
#include <core/kvector.h>

struct TestStruct {
    int id;
    const char* name;

    bool operator==(const TestStruct& other) const {
        return id == other.id && strcmp(name, other.name) == 0;
    }
};

DECLARE_UNIT_TEST("Vector Initialization", kvectorInitUnitTest) {
    kstl::vector<int> vec;

    ASSERT_EQ(vec.size(), 0, "Vector size should be 0 after initialization");
    ASSERT_EQ(vec.capacity(), 0, "Vector capacity should be 0 after initialization");
    ASSERT_TRUE(vec.empty(), "Vector should be empty after initialization");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector PushBack", kvectorPushBackUnitTest) {
    kstl::vector<int> vec;
    vec.pushBack(10);

    ASSERT_TRUE(vec.data() != nullptr, "Vector size should be 1 after pushBack");
    ASSERT_EQ(vec.size(), 1, "Vector size should be 1 after pushBack");
    ASSERT_EQ(vec[0], 10, "Element at index 0 should be 10");

    vec.pushBack(20);
    ASSERT_EQ(vec.size(), 2, "Vector size should be 2 after pushBack");
    ASSERT_EQ(vec[1], 20, "Element at index 1 should be 20");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector Capacity Growth", kvectorCapacityGrowthUnitTest) {
    kstl::vector<int> vec;
    size_t initialCapacity = vec.capacity();

    for (int i = 0; i < 100; ++i) {
        vec.pushBack(i);
    }

    ASSERT_EQ(vec.size(), 100, "Vector size should be 100 after 100 pushBack operations");
    ASSERT_TRUE(vec.capacity() > initialCapacity, "Vector capacity should grow after multiple insertions");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector Insert", kvectorInsertUnitTest) {
    kstl::vector<int> vec;
    vec.pushBack(10);
    vec.pushBack(30);
    
    vec.insert(1, 20);

    ASSERT_EQ(vec.size(), 3, "Vector size should be 3 after insert");
    ASSERT_EQ(vec[1], 20, "Element at index 1 should be 20 after insert");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector PopBack", kvectorPopBackUnitTest) {
    kstl::vector<int> vec;
    vec.pushBack(10);
    vec.pushBack(20);

    vec.popBack();
    ASSERT_EQ(vec.size(), 1, "Vector size should be 1 after popBack");
    ASSERT_EQ(vec[0], 10, "Element at index 0 should still be 10 after popBack");

    vec.popBack();
    ASSERT_EQ(vec.size(), 0, "Vector size should be 0 after second popBack");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector Erase", kvectorEraseUnitTest) {
    kstl::vector<int> vec;
    vec.pushBack(10);
    vec.pushBack(20);
    vec.pushBack(30);

    vec.erase(1);  // Remove the element at index 1 (20)

    ASSERT_EQ(vec.size(), 2, "Vector size should be 2 after erase");
    ASSERT_EQ(vec[0], 10, "Element at index 0 should still be 10");
    ASSERT_EQ(vec[1], 30, "Element at index 1 should now be 30");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector Clear", kvectorClearUnitTest) {
    kstl::vector<int> vec;
    vec.pushBack(10);
    vec.pushBack(20);

    vec.clear();

    ASSERT_EQ(vec.size(), 0, "Vector size should be 0 after clear");
    ASSERT_TRUE(vec.empty(), "Vector should be empty after clear");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector Find", kvectorFindUnitTest) {
    kstl::vector<int> vec;
    vec.pushBack(10);
    vec.pushBack(20);
    vec.pushBack(30);

    size_t index = vec.find(20);
    ASSERT_TRUE(index != kstl::vector<int>::npos, "Find should return a valid index");
    ASSERT_EQ(index, 1, "Element 20 should be at index 1");

    size_t notFoundIndex = vec.find(40);
    ASSERT_EQ(notFoundIndex, kstl::vector<int>::npos, "Find should return npos when element is not found");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector Copy Semantics", kvectorCopySemanticsUnitTest) {
    kstl::vector<int> vec1;
    vec1.pushBack(10);
    vec1.pushBack(20);

    kstl::vector<int> vec2 = vec1;

    ASSERT_EQ(vec2.size(), vec1.size(), "Copied vector should have the same size");
    ASSERT_TRUE(vec2[0] == vec1[0] && vec2[1] == vec1[1], "Copied vector should have the same values");

    vec2.pushBack(30);
    ASSERT_TRUE(vec1.size() != vec2.size(), "Original vector size should not be affected by copy");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector Size and Capacity Consistency", kvectorSizeCapacityConsistencyUnitTest) {
    kstl::vector<int> vec;
    
    for (int i = 0; i < 50; ++i) {
        vec.pushBack(i);
        ASSERT_TRUE(vec.size() <= vec.capacity(), "Vector size should never be greater than its capacity");
    }

    vec.insert(25, 100);
    ASSERT_TRUE(vec.size() <= vec.capacity(), "After insertion, size should never exceed capacity");

    vec.erase(10);
    ASSERT_TRUE(vec.size() <= vec.capacity(), "After erase, size should never exceed capacity");

    vec.clear();
    ASSERT_TRUE(vec.size() == 0, "After clear, vector size should be 0");
    ASSERT_TRUE(vec.capacity() > 0, "Capacity should not be 0 after clearing");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Vector with Custom Struct", kvectorCustomStructUnitTest) {
    kstl::vector<TestStruct> vec;

    TestStruct obj1 = {1, "Object 1"};
    TestStruct obj2 = {2, "Object 2"};
    TestStruct obj3 = {3, "Object 3"};

    // Test pushBack
    vec.pushBack(obj1);
    vec.pushBack(obj2);
    vec.pushBack(obj3);

    ASSERT_EQ(vec.size(), 3, "Vector size should be 3 after adding three elements");
    ASSERT_EQ(vec[0].id, obj1.id, "First element ID should match obj1");
    ASSERT_EQ(vec[1].id, obj2.id, "Second element ID should match obj2");
    ASSERT_EQ(vec[2].id, obj3.id, "Third element ID should match obj3");

    // Test erase
    vec.erase(1);  // Erase obj2 (index 1)
    ASSERT_EQ(vec.size(), 2, "Vector size should be 2 after erase");
    ASSERT_EQ(vec[0].id, obj1.id, "First element should still be obj1");
    ASSERT_EQ(vec[1].id, obj3.id, "Second element should now be obj3 after erase");

    // Test popBack
    vec.popBack();
    ASSERT_EQ(vec.size(), 1, "Vector size should be 1 after popBack");
    ASSERT_EQ(vec[0].id, obj1.id, "First element should still be obj1 after popBack");

    // Test clear
    vec.clear();
    ASSERT_EQ(vec.size(), 0, "Vector size should be 0 after clear");
    ASSERT_TRUE(vec.capacity() > 0, "Capacity should be greater than 0 after clearing");

    return UNIT_TEST_SUCCESS;
}
