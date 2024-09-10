#include "kernel_unit_tests.h"
#include <paging/page.h>
#include <memory/kmemory.h>

#define ALLOC_SIZE (PAGE_SIZE * 10)

DECLARE_UNIT_TEST("Heap Allocate Test", kheapAllocateUnitTest) {
    void* ptr = kmalloc(ALLOC_SIZE);
    ASSERT_TRUE_CRITICAL(ptr, "Allocated memory pointer was null");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Heap Allocate Aligned Test", kheapAllocateAlignedUnitTest) {
    void* ptr = kmalloc(ALLOC_SIZE);
    ASSERT_TRUE_CRITICAL(ptr, "Allocated memory pointer was null");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Heap Allocate and Free Test", kheapAllocateAndFreeUnitTest) {
    void* ptr = kmalloc(ALLOC_SIZE);
    ASSERT_TRUE_CRITICAL(ptr, "Allocated memory pointer was null");

    // Free the allocated memory
    kfree(ptr);

    void* ptr2 = kmalloc(ALLOC_SIZE);
    ASSERT_TRUE_CRITICAL(ptr2, "Allocated memory pointer was null");
    ASSERT_TRUE(ptr == ptr2, "Previously allocated and freed memory didn't get reused on a new allocation");

    return UNIT_TEST_SUCCESS;
}
