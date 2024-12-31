#include <unit_tests/unit_tests.h>
#include <memory/memory.h>
#include <kstl/vector.h>

using namespace kstl;

// A helper class to test construction/destruction
class VecTestObject {
public:
    static size_t alive_count;
    int value;

    VecTestObject(int val = 0) : value(val) {
        ++alive_count;
    }

    VecTestObject(const VecTestObject& other) : value(other.value) {
        ++alive_count;
    }

    VecTestObject(VecTestObject&& other) noexcept : value(other.value) {
        ++alive_count;
        // We could set other.value to something, but not strictly needed.
    }

    ~VecTestObject() {
        --alive_count;
    }

    VecTestObject& operator=(const VecTestObject& other) {
        if (this != &other) {
            value = other.value;
        }
        return *this;
    }

    bool operator==(const VecTestObject& other) const {
        return value == other.value;
    }
};
size_t VecTestObject::alive_count = 0;

// Test default construction
DECLARE_UNIT_TEST("vector default constructor", test_vector_default_constructor) {
    vector<int> v;
    ASSERT_TRUE(v.data() == nullptr, "Default constructed vector data should be nullptr");
    ASSERT_EQ(v.size(), (size_t)0, "Default constructed vector size should be 0");
    ASSERT_EQ(v.capacity(), (size_t)0, "Default constructed vector capacity should be 0");
    ASSERT_TRUE(v.empty(), "Default constructed vector should be empty");
    return UNIT_TEST_SUCCESS;
}

// Test construction with initial capacity
DECLARE_UNIT_TEST("vector constructor with capacity", test_vector_constructor_with_capacity) {
    vector<int> v(10);
    ASSERT_TRUE(v.data() != nullptr, "Vector with initial capacity should allocate data");
    ASSERT_EQ(v.size(), (size_t)0, "Size should be 0 initially");
    ASSERT_EQ(v.capacity(), (size_t)10, "Capacity should be 10");
    ASSERT_TRUE(v.empty(), "Should be empty initially");
    return UNIT_TEST_SUCCESS;
}

// Test push_back with a primitive type (int)
DECLARE_UNIT_TEST("vector push_back primitive", test_vector_push_back_primitive) {
    vector<int> v;
    for (int i = 0; i < 5; i++) {
        v.push_back(i);
    }

    ASSERT_EQ(v.size(), (size_t)5, "After pushing 5 elements, size should be 5");
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(v[i], i, "Elements should match pushed values");
    }

    // No manual cleanup needed since vector handles its own memory.
    return UNIT_TEST_SUCCESS;
}

// Test push_back with a non-primitive type
DECLARE_UNIT_TEST("vector push_back non-primitive", test_vector_push_back_non_primitive) {
    {
        vector<VecTestObject> v;
        ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "No objects alive before insertion");

        v.push_back(VecTestObject(42));
        ASSERT_EQ(v.size(), (size_t)1, "One element added");
        ASSERT_EQ(v[0].value, 42, "Element value should match");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)1, "One object alive after push_back");

        v.push_back(VecTestObject(99));
        ASSERT_EQ(v.size(), (size_t)2, "Two elements added");
        ASSERT_EQ(v[1].value, 99, "Element value should match");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)2, "Two objects alive");

        // Going out of scope will destroy the vector and all objects
    }
    ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "All objects should be destroyed after vector goes out of scope");
    return UNIT_TEST_SUCCESS;
}

// Test insert at various positions
DECLARE_UNIT_TEST("vector insert", test_vector_insert) {
    vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(4);

    // Insert in the middle
    v.insert(2, 3); // insert '3' at index 2
    ASSERT_EQ(v.size(), (size_t)4, "Inserting should increase size");
    ASSERT_EQ(v[0], 1, "Check element 0");
    ASSERT_EQ(v[1], 2, "Check element 1");
    ASSERT_EQ(v[2], 3, "Check inserted element");
    ASSERT_EQ(v[3], 4, "Check element after inserted one");

    // Insert at front
    v.insert(0, 0);
    ASSERT_EQ(v.size(), (size_t)5, "Inserting at front increases size");
    ASSERT_EQ(v[0], 0, "Front element");
    ASSERT_EQ(v[1], 1, "Shifted element");

    // Insert at end
    v.insert(v.size(), 5);
    ASSERT_EQ(v.size(), (size_t)6, "Insert at end increases size");
    ASSERT_EQ(v[5], 5, "Last element inserted");

    return UNIT_TEST_SUCCESS;
}

// Test pop_back
DECLARE_UNIT_TEST("vector pop_back", test_vector_pop_back) {
    vector<int> v;
    v.push_back(10);
    v.push_back(20);

    ASSERT_EQ(v.size(), (size_t)2, "Two elements before pop");
    v.pop_back();
    ASSERT_EQ(v.size(), (size_t)1, "One element after pop");
    ASSERT_EQ(v[0], 10, "Remaining element should be the first one");
    v.pop_back();
    ASSERT_EQ(v.size(), (size_t)0, "No elements after popping again");
    ASSERT_TRUE(v.empty(), "Vector should be empty");

    return UNIT_TEST_SUCCESS;
}

// Test erase
DECLARE_UNIT_TEST("vector erase", test_vector_erase) {
    vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    v.push_back(40);

    // Erase middle element
    v.erase(1); // remove element at index 1 (which was 20)
    ASSERT_EQ(v.size(), (size_t)3, "One element removed");
    ASSERT_EQ(v[0], 10, "Check element 0");
    ASSERT_EQ(v[1], 30, "Check shifted element");
    ASSERT_EQ(v[2], 40, "Check last element remains same");

    // Erase first element
    v.erase(0);
    ASSERT_EQ(v.size(), (size_t)2, "Now two elements left");
    ASSERT_EQ(v[0], 30, "30 should now be at front");
    ASSERT_EQ(v[1], 40, "40 after 30");

    // Erase last element
    v.erase(1);
    ASSERT_EQ(v.size(), (size_t)1, "Now one element left");
    ASSERT_EQ(v[0], 30, "30 remains");

    // Erase out of range (should do nothing)
    v.erase(5);
    ASSERT_EQ(v.size(), (size_t)1, "No change after erasing out of range index");

    return UNIT_TEST_SUCCESS;
}

// Test find
DECLARE_UNIT_TEST("vector find", test_vector_find) {
    vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);

    ASSERT_EQ(v.find(20), (size_t)1, "Should find 20 at index 1");
    ASSERT_EQ(v.find(40), vector<int>::npos, "Should not find 40");
    ASSERT_EQ(v.find(10), (size_t)0, "Should find 10 at index 0");
    ASSERT_EQ(v.find(30), (size_t)2, "Should find 30 at index 2");

    return UNIT_TEST_SUCCESS;
}

// Test copy constructor
DECLARE_UNIT_TEST("vector copy constructor", test_vector_copy_constructor) {
    {
        vector<int> v;
        v.push_back(1);
        v.push_back(2);

        vector<int> v2 = v; // Copy construct
        ASSERT_EQ(v2.size(), v.size(), "Copied vector should have same size");
        ASSERT_EQ(v2[0], 1, "Check element");
        ASSERT_EQ(v2[1], 2, "Check element");

        // Modify original should not affect copy
        v[0] = 10;
        ASSERT_EQ(v2[0], 1, "Copy should be independent of original after construction");
    }

    {
        // Test with non-primitive
        ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "No objects alive before test");
        vector<VecTestObject> v;
        v.push_back(VecTestObject(42));
        v.push_back(VecTestObject(7));
        ASSERT_EQ(VecTestObject::alive_count, (size_t)2, "Two objects alive");

        vector<VecTestObject> v2 = v; // copy
        ASSERT_EQ(v2.size(), (size_t)2, "Copied vector size");
        ASSERT_EQ(v2[0].value, 42, "Check value");
        ASSERT_EQ(v2[1].value, 7,  "Check value");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)4, "Copy constructor should create two new objects");

        // Both vectors go out of scope here, destroying all 4 objects
    }
    ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "All objects destroyed after scope");
    return UNIT_TEST_SUCCESS;
}

// Test copy assignment operator
DECLARE_UNIT_TEST("vector copy assignment", test_vector_copy_assignment) {
    vector<int> v;
    v.push_back(1);
    v.push_back(2);

    vector<int> v2;
    v2.push_back(10);
    v2 = v; // copy assignment

    ASSERT_EQ(v2.size(), (size_t)2, "Copied size");
    ASSERT_EQ(v2[0], 1, "Check element");
    ASSERT_EQ(v2[1], 2, "Check element");

    // Modify v2, not affecting v
    v2[0] = 99;
    ASSERT_EQ(v[0], 1, "Original should remain unchanged");
    return UNIT_TEST_SUCCESS;
}

// Test move constructor
DECLARE_UNIT_TEST("vector move constructor", test_vector_move_constructor) {
    vector<int> v;
    v.push_back(10);
    v.push_back(20);

    vector<int> v2((vector<int>&&)v); // move construct
    ASSERT_EQ(v2.size(), (size_t)2, "Moved vector should have size 2");
    ASSERT_EQ(v2[0], 10, "Check element 0");
    ASSERT_EQ(v2[1], 20, "Check element 1");
    ASSERT_EQ(v.size(), (size_t)0, "Original should be empty after move");
    ASSERT_TRUE(v.data() == nullptr, "Original data should be null after move");
    return UNIT_TEST_SUCCESS;
}

// Test move assignment
DECLARE_UNIT_TEST("vector move assignment", test_vector_move_assignment) {
    vector<int> v;
    v.push_back(10);
    v.push_back(20);

    vector<int> v2;
    v2.push_back(1);
    v2 = (vector<int>&&)v; // move assign

    ASSERT_EQ(v2.size(), (size_t)2, "Moved vector should have size 2");
    ASSERT_EQ(v2[0], 10, "Check element");
    ASSERT_EQ(v2[1], 20, "Check element");
    ASSERT_EQ(v.size(), (size_t)0, "Original should be empty after move");
    ASSERT_TRUE(v.data() == nullptr, "Original data should be null after move assignment");
    return UNIT_TEST_SUCCESS;
}

// Test reserve
DECLARE_UNIT_TEST("vector reserve", test_vector_reserve) {
    vector<int> v;
    v.push_back(1);
    v.push_back(2);

    size_t old_size = v.size();
    v.reserve(10);
    ASSERT_EQ(v.capacity(), (size_t)10, "Capacity should increase to 10");
    ASSERT_EQ(v.size(), old_size, "Size should remain the same after reserve");
    ASSERT_EQ(v[0], 1, "Elements should remain intact");
    ASSERT_EQ(v[1], 2, "Elements should remain intact");

    return UNIT_TEST_SUCCESS;
}

// Test resize with primitive and non-primitive types
DECLARE_UNIT_TEST("vector resize", test_vector_resize) {
    // Test with primitive type
    {
        vector<int> v;
        v.push_back(1);
        v.push_back(2);

        // Resize to a larger size
        v.resize(5);
        ASSERT_EQ(v.size(), (size_t)5, "Resized vector should have size 5");
        ASSERT_EQ(v[0], 1, "Check existing element 0");
        ASSERT_EQ(v[1], 2, "Check existing element 1");
        ASSERT_EQ(v[2], 0, "Newly added elements should be default-initialized to 0");
        ASSERT_EQ(v[3], 0, "Newly added elements should be default-initialized to 0");
        ASSERT_EQ(v[4], 0, "Newly added elements should be default-initialized to 0");

        // Resize to a smaller size
        v.resize(1);
        ASSERT_EQ(v.size(), (size_t)1, "Resized vector should have size 1");
        ASSERT_EQ(v[0], 1, "Remaining element should be the first one");

        // Resize to zero
        v.resize(0);
        ASSERT_EQ(v.size(), (size_t)0, "Resized vector should have size 0");
        ASSERT_TRUE(v.empty(), "Vector should be empty");
    }

    // Test with non-primitive type
    {
        ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "No objects alive at start");

        vector<VecTestObject> v;
        v.push_back(VecTestObject(10));
        v.push_back(VecTestObject(20));

        ASSERT_EQ(v.size(), (size_t)2, "Initial size should be 2");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)2, "Two objects alive");

        // Resize to a larger size
        v.resize(5);
        ASSERT_EQ(v.size(), (size_t)5, "Resized vector should have size 5");
        ASSERT_EQ(v[0].value, 10, "Check existing element 0");
        ASSERT_EQ(v[1].value, 20, "Check existing element 1");
        ASSERT_EQ(v[2].value, 0, "Newly added elements should be default-constructed");
        ASSERT_EQ(v[3].value, 0, "Newly added elements should be default-constructed");
        ASSERT_EQ(v[4].value, 0, "Newly added elements should be default-constructed");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)5, "Five objects alive after resize");

        // Resize to a smaller size
        v.resize(2);
        ASSERT_EQ(v.size(), (size_t)2, "Resized vector should have size 2");
        ASSERT_EQ(v[0].value, 10, "Check remaining element 0");
        ASSERT_EQ(v[1].value, 20, "Check remaining element 1");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)2, "Two objects alive after resizing smaller");

        // Resize to zero
        v.resize(0);
        ASSERT_EQ(v.size(), (size_t)0, "Resized vector should have size 0");
        ASSERT_TRUE(v.empty(), "Vector should be empty");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "No objects alive after resizing to zero");
    }

    return UNIT_TEST_SUCCESS;
}

// Test clear
DECLARE_UNIT_TEST("vector clear", test_vector_clear) {
    vector<int> v;
    v.push_back(1);
    v.push_back(2);

    v.clear();
    ASSERT_EQ(v.size(), (size_t)0, "Size should be 0 after clear");
    ASSERT_TRUE(v.data() != nullptr, "Data may still be allocated, capacity unchanged");
    ASSERT_EQ(v.capacity(), (size_t)2, "Capacity should remain");
    ASSERT_TRUE(v.empty(), "Should be empty after clear");

    return UNIT_TEST_SUCCESS;
}

// Test with multiple insertions, erasures and check destructors with VecTestObject
DECLARE_UNIT_TEST("vector complex VecTestObject operations", test_vector_complex_VecTestObject) {
    ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "No objects alive at start");

    {
        vector<VecTestObject> v;
        v.push_back(VecTestObject(10));
        v.push_back(VecTestObject(20));
        v.push_back(VecTestObject(30));

        ASSERT_EQ(v.size(), (size_t)3, "Three elements");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)3, "Three alive objects");

        v.insert(1, VecTestObject(15));
        ASSERT_EQ(v.size(), (size_t)4, "Inserted one element");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)4, "Four objects alive");
        ASSERT_EQ(v[1].value, 15, "Check inserted value");

        v.erase(2); // erase element originally at index 2 (which was 20 or 30 depending on shift)
        ASSERT_EQ(v.size(), (size_t)3, "One less element after erase");
        // After erase, we should have destroyed one object
        ASSERT_EQ(VecTestObject::alive_count, (size_t)3, "Three objects alive after erase");

        // Let's clear and see if all objects are destroyed
        v.clear();
        ASSERT_EQ(v.size(), (size_t)0, "Cleared vector");
        ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "No objects alive after clear");
    }

    ASSERT_EQ(VecTestObject::alive_count, (size_t)0, "No objects alive after vector scope ends");
    return UNIT_TEST_SUCCESS;
}
