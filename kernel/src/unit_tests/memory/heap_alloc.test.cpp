#include <unit_tests/unit_tests.h>
#include <memory/memory.h>

// A simple structure to test `new` and `delete`
class HeapAllocTestObject {
public:
    static size_t live_count;
    int value;

    HeapAllocTestObject(int v = 0) : value(v) {
        ++live_count;
    }
    HeapAllocTestObject(const HeapAllocTestObject& other) : value(other.value) {
        ++live_count;
    }
    ~HeapAllocTestObject() {
        --live_count;
    }
};

size_t HeapAllocTestObject::live_count = 0;

// Utility functions
static bool memory_is_zeroed(void* ptr, size_t size) {
    unsigned char* cptr = (unsigned char*)ptr;
    for (size_t i = 0; i < size; i++) {
        if (cptr[i] != 0) return false;
    }
    return true;
}

// Test malloc and free
DECLARE_UNIT_TEST("heap malloc/free basic", test_heap_malloc_free_basic) {
    void* ptr = malloc(64);
    ASSERT_TRUE(ptr != nullptr, "malloc(64) should return non-null pointer");

    // Write something into the allocated memory
    unsigned char* cptr = (unsigned char*)ptr;
    for (int i = 0; i < 64; i++) {
        cptr[i] = (unsigned char)i;
    }

    // Free the allocated memory
    free(ptr);

    // Allocation again should succeed
    void* ptr2 = malloc(64);
    ASSERT_TRUE(ptr2 != nullptr, "malloc(64) again should return non-null pointer");
    free(ptr2);

    return UNIT_TEST_SUCCESS;
}

// Test zmalloc ensures memory is zeroed
DECLARE_UNIT_TEST("heap zmalloc zeroed memory", test_heap_zmalloc_zeroed) {
    size_t size = 128;
    void* ptr = zmalloc(size);
    ASSERT_TRUE(ptr != nullptr, "zmalloc(128) should return non-null pointer");
    ASSERT_TRUE(memory_is_zeroed(ptr, size), "zmalloc should return zero-initialized memory");
    free(ptr);
    return UNIT_TEST_SUCCESS;
}

// Test malloc(0) behavior
DECLARE_UNIT_TEST("heap malloc(0)", test_heap_malloc_zero) {
    void* ptr = malloc(0);
    ASSERT_TRUE(ptr == nullptr, "malloc(0) should return a null pointer");
    return UNIT_TEST_SUCCESS;
}

// Test realloc: shrink and grow
DECLARE_UNIT_TEST("heap realloc shrink and grow", test_heap_realloc) {
    size_t initial_size = 50;
    char* ptr = (char*)malloc(initial_size);
    ASSERT_TRUE(ptr != nullptr, "malloc(50) should succeed");

    // Fill with a known pattern
    for (int i = 0; i < (int)initial_size; i++) {
        ptr[i] = (char)(i % 128);
    }

    // Shrink the block
    size_t smaller_size = 20;
    char* shrunk = (char*)realloc(ptr, smaller_size);
    ASSERT_TRUE(shrunk != nullptr, "realloc to smaller size should succeed");
    for (int i = 0; i < (int)smaller_size; i++) {
        ASSERT_EQ((int)shrunk[i], (int)(i % 128), "Data should be preserved after shrinking");
    }

    // Grow the block
    size_t larger_size = 100;
    char* grown = (char*)realloc(shrunk, larger_size);
    ASSERT_TRUE(grown != nullptr, "realloc to larger size should succeed");
    for (int i = 0; i < (int)smaller_size; i++) {
        ASSERT_EQ((int)grown[i], (int)(i % 128), "Old data should be preserved after growing");
    }

    // The extra space after smaller_size may be uninitialized, we won't check that here.
    free(grown);
    return UNIT_TEST_SUCCESS;
}

// Test realloc from NULL
DECLARE_UNIT_TEST("heap realloc from NULL", test_heap_realloc_from_null) {
    void* ptr = realloc(nullptr, 64);
    ASSERT_TRUE(ptr != nullptr, "realloc(nullptr, 64) should act like malloc(64)");
    free(ptr);
    return UNIT_TEST_SUCCESS;
}

// Test multiple allocations and frees
DECLARE_UNIT_TEST("heap multiple allocations", test_heap_multiple_allocations) {
    // Allocate multiple blocks and store them
    const int num_blocks = 10;
    void* blocks[num_blocks];

    for (int i = 0; i < num_blocks; i++) {
        blocks[i] = malloc(32);
        ASSERT_TRUE(blocks[i] != nullptr, "malloc(32) should succeed for multiple blocks");
    }

    // Free them in reverse order
    for (int i = num_blocks - 1; i >= 0; i--) {
        free(blocks[i]);
    }
    return UNIT_TEST_SUCCESS;
}

// Test operator new and delete for a primitive type
DECLARE_UNIT_TEST("heap operator new/delete primitive", test_operator_new_delete_primitive) {
    int* ptr = new int(42);
    ASSERT_TRUE(ptr != nullptr, "new int(42) should return non-null");
    ASSERT_EQ(*ptr, 42, "Value should be initialized to 42");
    delete ptr;
    return UNIT_TEST_SUCCESS;
}

// Test operator new and delete for an object
DECLARE_UNIT_TEST("heap operator new/delete object", test_operator_new_delete_object) {
    ASSERT_EQ(HeapAllocTestObject::live_count, (size_t)0, "No HeapAllocTestObjects alive at start");

    HeapAllocTestObject* obj = new HeapAllocTestObject(100);
    ASSERT_TRUE(obj != nullptr, "new HeapAllocTestObject(100) should succeed");
    ASSERT_EQ(HeapAllocTestObject::live_count, (size_t)1, "One object should be alive");
    ASSERT_EQ(obj->value, 100, "Object value should be initialized properly");

    delete obj;
    ASSERT_EQ(HeapAllocTestObject::live_count, (size_t)0, "Object should be destroyed");
    return UNIT_TEST_SUCCESS;
}

// Test new[] and delete[]
DECLARE_UNIT_TEST("heap operator new[]/delete[]", test_operator_new_array_delete_array) {
    ASSERT_EQ(HeapAllocTestObject::live_count, (size_t)0, "No objects alive at start");

    size_t count = 5;
    HeapAllocTestObject* arr = new HeapAllocTestObject[count];
    ASSERT_TRUE(arr != nullptr, "new HeapAllocTestObject[5] should succeed");
    ASSERT_EQ(HeapAllocTestObject::live_count, (size_t)5, "Five objects should be alive for array allocation");

    // Assign values
    for (size_t i = 0; i < count; i++) {
        arr[i].value = (int)i;
    }

    // Check values
    for (size_t i = 0; i < count; i++) {
        ASSERT_EQ(arr[i].value, (int)i, "Check assigned values");
    }

    delete[] arr;
    ASSERT_EQ(HeapAllocTestObject::live_count, (size_t)0, "All array objects should be destroyed");
    return UNIT_TEST_SUCCESS;
}

// Test operator delete(ptr, size) (sized delete) - C++14 and beyond
DECLARE_UNIT_TEST("heap operator delete(ptr, size)", test_operator_delete_sized) {
    int* ptr = new int(123);
    ASSERT_TRUE(ptr != nullptr, "new int(123) should succeed");
    ASSERT_EQ(*ptr, 123, "Check value");

    // The sized delete operator is called automatically for array delete, or can be called manually.
    // For a single object, it's typically the same as operator delete(void*).
    // We'll just call it explicitly to ensure no crash.
    operator delete(ptr, sizeof(int));

    return UNIT_TEST_SUCCESS;
}

// Test large allocation
DECLARE_UNIT_TEST("heap large allocation", test_heap_large_allocation) {
    // Allocate a large block of memory, e.g., 64 KB
    size_t large_size = 64 * 1024;
    void* ptr = malloc(large_size);
    ASSERT_TRUE(ptr != nullptr, "malloc(64KB) should succeed");
    // Just do a basic write check
    unsigned char* c = (unsigned char*)ptr;
    c[0] = 0xAB;
    c[large_size - 1] = 0xCD;
    ASSERT_EQ(c[0], (unsigned char)0xAB, "First byte should be written");
    ASSERT_EQ(c[large_size - 1], (unsigned char)0xCD, "Last byte should be written");

    free(ptr);
    return UNIT_TEST_SUCCESS;
}

// Test exhaustive amount of allocations
DECLARE_UNIT_TEST("heap allocation exhaustion", test_heap_allocation_exhaustion) {
    const size_t max_attempts = 3000;
    void** blocks = (void**)zmalloc(max_attempts * sizeof(void*));
    ASSERT_TRUE(blocks != nullptr, "Should be able to allocate temporary array for tracking");

    size_t allocated_count = 0;
    for (size_t i = 0; i < max_attempts; i++) {
        void* p = malloc(1024); // Allocate 1KB blocks repeatedly
        if (!p) {
            // Allocation failed, stop
            break;
        }
        blocks[allocated_count++] = p;
    }

    serial::printf("[INFO] Allocated %llu blocks of 1KB before failure or max.\n", allocated_count);
    ASSERT_TRUE(allocated_count > 0, "Should have allocated at least one block");

    // Free all allocated memory
    for (size_t i = 0; i < allocated_count; i++) {
        free(blocks[i]);
    }

    free(blocks);

    // Try allocating again to ensure heap is still functional
    void* test_ptr = malloc(64);
    ASSERT_TRUE(test_ptr != nullptr, "Should still be able to allocate after exhaustion test");
    free(test_ptr);

    return UNIT_TEST_SUCCESS;
}
