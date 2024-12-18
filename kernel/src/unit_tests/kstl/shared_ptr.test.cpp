#include <unit_tests/unit_tests.h>
#include <memory/memory.h>

using kstl::shared_ptr;
using kstl::make_shared;

// Test default constructor
DECLARE_UNIT_TEST("shared_ptr default constructor", test_shared_ptr_default_constructor) {
    shared_ptr<int> sp;
    ASSERT_EQ(sp.get(), (int*)nullptr, "Default constructed shared_ptr should hold nullptr");
    ASSERT_EQ(sp.ref_count(), (size_t)0, "Default constructed shared_ptr should have ref_count 0");
    return UNIT_TEST_SUCCESS; // Resources are released automatically.
}

// Test constructor with a raw pointer
DECLARE_UNIT_TEST("shared_ptr constructor with pointer", test_shared_ptr_constructor_with_ptr) {
    int* raw_ptr = new int(42); // allocated resource
    shared_ptr<int> sp(raw_ptr);

    ASSERT_TRUE(sp.get() == raw_ptr, "shared_ptr should hold the passed pointer");
    ASSERT_EQ(sp.ref_count(), (size_t)1, "Ref count should be 1 after construction with pointer");
    ASSERT_EQ(*sp, 42, "Dereferenced value should match the constructed value");

    // No need to manually delete raw_ptr; shared_ptr will handle it upon destruction.
    return UNIT_TEST_SUCCESS;
}

// Test copy constructor
DECLARE_UNIT_TEST("shared_ptr copy constructor", test_shared_ptr_copy_constructor) {
    shared_ptr<int> sp = make_shared<int>(42);
    ASSERT_EQ(sp.ref_count(), (size_t)1, "Ref count should be 1 after make_shared");

    shared_ptr<int> sp2(sp); // copy construction
    ASSERT_EQ(sp.ref_count(), (size_t)2, "Ref count should be 2 after copying");
    ASSERT_EQ(sp2.ref_count(), (size_t)2, "Both copies should report the same ref_count");
    ASSERT_EQ(*sp2, 42, "Dereferenced value in copy should match original");

    return UNIT_TEST_SUCCESS;
}

// Test copy assignment operator
DECLARE_UNIT_TEST("shared_ptr copy assignment", test_shared_ptr_copy_assignment) {
    shared_ptr<int> sp = make_shared<int>(42);
    ASSERT_EQ(sp.ref_count(), (size_t)1, "make_shared should create a single reference");

    shared_ptr<int> sp2 = make_shared<int>(100);
    ASSERT_EQ(sp2.ref_count(), (size_t)1, "Second shared_ptr distinct allocation should have ref_count 1");

    sp2 = sp; // copy assignment
    ASSERT_EQ(sp.ref_count(), (size_t)2, "After assignment, ref_count should increase");
    ASSERT_EQ(sp2.ref_count(), (size_t)2, "Both now point to the same resource");
    ASSERT_EQ(*sp2, 42, "Assigned pointer should now hold the original's value");

    return UNIT_TEST_SUCCESS;
}

// Test move constructor
DECLARE_UNIT_TEST("shared_ptr move constructor", test_shared_ptr_move_constructor) {
    shared_ptr<int> sp = make_shared<int>(42);
    ASSERT_EQ(sp.ref_count(), (size_t)1, "Initial ref_count should be 1");

    // Move construction: cast sp to rvalue reference
    shared_ptr<int> sp2((shared_ptr<int>&&)sp);
    ASSERT_TRUE(sp.get() == nullptr, "After move, the original shared_ptr should hold nullptr");
    ASSERT_EQ(sp.ref_count(), (size_t)0, "After move, original ref_count should be 0");
    ASSERT_TRUE(sp2.get() != nullptr, "Moved-to shared_ptr should hold the resource");
    ASSERT_EQ(sp2.ref_count(), (size_t)1, "Moved-to shared_ptr should have ref_count of 1");
    ASSERT_EQ(*sp2, 42, "Moved resource should retain the value");

    return UNIT_TEST_SUCCESS;
}

// Test move assignment operator
DECLARE_UNIT_TEST("shared_ptr move assignment", test_shared_ptr_move_assignment) {
    shared_ptr<int> sp = make_shared<int>(42);
    ASSERT_EQ(sp.ref_count(), (size_t)1, "Initial ref_count should be 1");

    shared_ptr<int> sp2;
    sp2 = (shared_ptr<int>&&)sp; // move assignment
    ASSERT_TRUE(sp.get() == nullptr, "After move assignment, the original should hold nullptr");
    ASSERT_EQ(sp.ref_count(), (size_t)0, "Original should now have ref_count 0");
    ASSERT_TRUE(sp2.get() != nullptr, "Moved-to shared_ptr should hold the resource");
    ASSERT_EQ(sp2.ref_count(), (size_t)1, "Moved-to shared_ptr should have ref_count of 1");
    ASSERT_EQ(*sp2, 42, "Moved resource should retain the value");

    return UNIT_TEST_SUCCESS;
}

// Test equality and inequality operators
DECLARE_UNIT_TEST("shared_ptr equality operators", test_shared_ptr_equality_operators) {
    shared_ptr<int> sp = make_shared<int>(42);
    shared_ptr<int> sp2 = sp;
    // sp and sp2 share the same resource
    ASSERT_TRUE(sp == sp2, "Two shared_ptrs to the same resource should be equal");
    ASSERT_TRUE(!(sp != sp2), "Negation of equality should be false");

    shared_ptr<int> sp3 = make_shared<int>(42);
    // sp3 points to a different resource, even though the value is the same
    ASSERT_TRUE(sp != sp3, "Two shared_ptrs to different resources should not be equal");
    ASSERT_TRUE(!(sp == sp3), "They should not be considered equal");

    // Compare with raw pointers
    int* raw_ptr = sp.get();
    ASSERT_TRUE(sp == raw_ptr, "shared_ptr should equal its underlying pointer");
    ASSERT_TRUE(!(sp != raw_ptr), "Negation should be false");

    // Different pointer
    int* another_raw_ptr = sp3.get();
    ASSERT_TRUE(sp != another_raw_ptr, "shared_ptr should not equal a different raw pointer");
    ASSERT_TRUE(!(sp == another_raw_ptr), "They should not be equal");

    return UNIT_TEST_SUCCESS;
}

// Test ref_count behavior across multiple copies
DECLARE_UNIT_TEST("shared_ptr multiple reference count", test_shared_ptr_multiple_references) {
    shared_ptr<int> sp1 = make_shared<int>(42);
    ASSERT_EQ(sp1.ref_count(), (size_t)1, "make_shared should start with ref_count 1");

    shared_ptr<int> sp2 = sp1;
    ASSERT_EQ(sp1.ref_count(), (size_t)2, "After copy, ref_count should be 2");
    ASSERT_EQ(sp2.ref_count(), (size_t)2, "Both should show the same ref_count");

    shared_ptr<int> sp3;
    sp3 = sp1;
    ASSERT_EQ(sp1.ref_count(), (size_t)3, "After another copy, ref_count should be 3");
    ASSERT_EQ(sp2.ref_count(), (size_t)3, "All copies should reflect ref_count 3");
    ASSERT_EQ(sp3.ref_count(), (size_t)3, "All copies should reflect ref_count 3");

    // When this test function returns, sp1, sp2, and sp3 all go out of scope.
    // The resource should be freed exactly once, ensuring no memory leaks.
    return UNIT_TEST_SUCCESS;
}

// Test that reassigning a shared_ptr to a default one releases the resource
DECLARE_UNIT_TEST("shared_ptr release by reassignment", test_shared_ptr_release_reassignment) {
    shared_ptr<int> sp = make_shared<int>(42);
    ASSERT_EQ(sp.ref_count(), (size_t)1, "Initial ref_count should be 1");

    sp = shared_ptr<int>(); // Assign a default constructed shared_ptr
    ASSERT_EQ(sp.get(), (int*)nullptr, "After reassignment, sp should hold nullptr");
    ASSERT_EQ(sp.ref_count(), (size_t)0, "After reassignment, ref_count should be 0");

    // The old resource should have been released here.
    return UNIT_TEST_SUCCESS;
}

// Test make_shared functionality
DECLARE_UNIT_TEST("make_shared functionality", test_make_shared) {
    shared_ptr<int> sp = make_shared<int>(123);
    ASSERT_TRUE(sp.get() != nullptr, "make_shared should return a valid pointer");
    ASSERT_EQ(*sp, 123, "Dereferenced value should match the constructed value");
    ASSERT_EQ(sp.ref_count(), (size_t)1, "Ref_count should be 1 after make_shared");

    {
        shared_ptr<int> sp2 = sp;
        ASSERT_EQ(sp.ref_count(), (size_t)2, "Ref_count should be 2 with a second reference in scope");
        ASSERT_EQ(sp2.ref_count(), (size_t)2, "Both should report the same ref_count");
    }

    // After sp2 is out of scope, ref_count should drop back down to 1
    ASSERT_EQ(sp.ref_count(), (size_t)1, "Ref_count should return to 1 after sp2 goes out of scope");

    return UNIT_TEST_SUCCESS;
}
