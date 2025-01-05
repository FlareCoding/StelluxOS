#include <unit_tests/unit_tests.h>
#include <kstl/hashmap.h>

using namespace kstl;

// Utility function to compare values for assertion.
template <typename T>
static bool values_equal(const T& lhs, const T& rhs) {
    return lhs == rhs;
}

// Test default constructor
DECLARE_UNIT_TEST("hashmap default constructor", test_hashmap_default_constructor) {
    hashmap<uint64_t, bool> map;
    ASSERT_EQ(map.size(), (size_t)0, "Default constructed hashmap should have size 0");
    return UNIT_TEST_SUCCESS;
}

// Test insert and retrieve
DECLARE_UNIT_TEST("hashmap insert and retrieve", test_hashmap_insert_retrieve) {
    hashmap<uint64_t, bool> map;
    ASSERT_TRUE(map.insert(42, true), "Insert key 42 should succeed");
    ASSERT_TRUE(map.insert(84, false), "Insert key 84 should succeed");
    
    bool* value = map.get(42);
    ASSERT_TRUE(value != nullptr && *value == true, "Key 42 should retrieve true");
    
    value = map.get(84);
    ASSERT_TRUE(value != nullptr && *value == false, "Key 84 should retrieve false");
    
    value = map.get(99);
    ASSERT_TRUE(value == nullptr, "Non-existent key 99 should return nullptr");
    return UNIT_TEST_SUCCESS;
}

// Test duplicate insert
DECLARE_UNIT_TEST("hashmap duplicate insert", test_hashmap_duplicate_insert) {
    hashmap<uint64_t, int> map;
    ASSERT_TRUE(map.insert(100, 1), "Insert key 100 should succeed");
    ASSERT_FALSE(map.insert(100, 2), "Duplicate insert for key 100 should fail");
    
    int* value = map.get(100);
    ASSERT_TRUE(value != nullptr && *value == 1, "Value for key 100 should remain unchanged");
    return UNIT_TEST_SUCCESS;
}

// Test remove
DECLARE_UNIT_TEST("hashmap remove", test_hashmap_remove) {
    hashmap<uint64_t, int> map;
    map.insert(10, 100);
    map.insert(20, 200);
    
    ASSERT_TRUE(map.remove(10), "Remove key 10 should succeed");
    ASSERT_TRUE(map.get(10) == nullptr, "Key 10 should no longer exist");
    
    ASSERT_FALSE(map.remove(99), "Remove non-existent key 99 should fail");
    ASSERT_TRUE(map.get(20) != nullptr && *map.get(20) == 200, "Key 20 should still exist");
    return UNIT_TEST_SUCCESS;
}

// Test operator[]
DECLARE_UNIT_TEST("hashmap operator[]", test_hashmap_operator_subscript) {
    hashmap<int, int> map;
    map[1] = 100;
    ASSERT_EQ(map[1], 100, "Operator[] should allow insertion");
    
    map[1] = 200;
    ASSERT_EQ(map[1], 200, "Operator[] should allow modification");
    
    ASSERT_EQ(map[2], 0, "Operator[] should default-construct non-existent keys");
    return UNIT_TEST_SUCCESS;
}

// Test find
DECLARE_UNIT_TEST("hashmap find", test_hashmap_find) {
    hashmap<uint64_t, bool> map;
    map.insert(500, true);
    ASSERT_TRUE(map.find(500), "Find should return true for existing key 500");
    ASSERT_FALSE(map.find(999), "Find should return false for non-existent key 999");
    return UNIT_TEST_SUCCESS;
}

// Test dynamic resizing
DECLARE_UNIT_TEST("hashmap dynamic resizing", test_hashmap_dynamic_resizing) {
    hashmap<int, int> map(4, 0.75); // Small initial size to force rehashing
    for (int i = 0; i < 20; ++i) {
        map.insert(i, i * 10);
    }
    ASSERT_EQ(map.size(), (size_t)20, "All keys should be inserted successfully");

    for (int i = 0; i < 20; ++i) {
        int* value = map.get(i);
        ASSERT_TRUE(value != nullptr && *value == i * 10, "All inserted keys should be retrievable");
    }
    return UNIT_TEST_SUCCESS;
}

// Test complex types
DECLARE_UNIT_TEST("hashmap complex types", test_hashmap_complex_types) {
    struct ComplexKey {
        int a, b;
        bool operator==(const ComplexKey& other) const {
            return a == other.a && b == other.b;
        }
    };

    struct HashComplexKey {
        size_t operator()(const ComplexKey& key) const {
            return (key.a * 31) + key.b;
        }
    };

    hashmap<ComplexKey, int> map;
    ComplexKey k1{1, 2};
    ComplexKey k2{3, 4};

    ASSERT_TRUE(map.insert(k1, 100), "Insert complex key {1, 2} should succeed");
    ASSERT_TRUE(map.insert(k2, 200), "Insert complex key {3, 4} should succeed");

    ASSERT_EQ(map[k1], 100, "Value for complex key {1, 2} should be retrievable");
    ASSERT_EQ(map[k2], 200, "Value for complex key {3, 4} should be retrievable");
    return UNIT_TEST_SUCCESS;
}

// Test collision handling
DECLARE_UNIT_TEST("hashmap collision handling", test_hashmap_collision_handling) {
    hashmap<int, int> map(2, 0.75); // Force collisions with small bucket count
    map.insert(1, 10);
    map.insert(3, 30); // Both keys should hash to the same bucket

    ASSERT_EQ(map[1], 10, "Value for key 1 should be retrievable");
    ASSERT_EQ(map[3], 30, "Value for key 3 should be retrievable");
    return UNIT_TEST_SUCCESS;
}

// Test edge case with empty hashmap
DECLARE_UNIT_TEST("hashmap empty edge case", test_hashmap_empty) {
    hashmap<int, int> map;
    ASSERT_EQ(map.size(), (size_t)0, "Empty hashmap should have size 0");
    ASSERT_TRUE(map.get(1) == nullptr, "Empty hashmap should return nullptr for any key");
    ASSERT_FALSE(map.remove(1), "Remove on empty hashmap should fail");
    return UNIT_TEST_SUCCESS;
}
