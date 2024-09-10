#include "kernel_unit_tests.h"
#include <paging/page.h>
#include <memory/kmemory.h>

#define ALLOC_SIZE (PAGE_SIZE * 10)

DECLARE_UNIT_TEST("Heap Allocate Test", kheapAllocateUnitTest) {
    void* ptr = kmalloc(ALLOC_SIZE);
    ASSERT_TRUE_CRITICAL(ptr, "Allocated memory pointer was null");

    // Free the memory on success
    kfree(ptr);

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Heap Allocate Aligned Test", kheapAllocateAlignedUnitTest) {
    // Define sizes and alignments to test
    size_t sizes[] = { 1024, 2048, 4096 };
    size_t alignments[] = { 16, 32, 64, 128, 256 };
    
    // Loop through each size and alignment combination
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (size_t j = 0; j < sizeof(alignments) / sizeof(alignments[0]); j++) {
            // Allocate memory with the given size and alignment
            void* ptr = kmallocAligned(sizes[i], alignments[j]);
            
            // Assert that the pointer is not null (allocation was successful)
            ASSERT_TRUE_CRITICAL(ptr, "Allocated memory pointer was null");

            // Check if the pointer is correctly aligned by verifying it's divisible by the alignment
            ASSERT_TRUE_CRITICAL(((uint64_t)ptr % alignments[j]) == 0, 
                "Memory was not correctly aligned to the requested boundary");
            
            kuPrint(UNIT_TEST "Allocated %llu bytes with alignment %llu: Success\n", (uint64_t)sizes[i], (uint64_t)alignments[j]);

            // Optionally: Free the allocated memory (if applicable)
            kfree(ptr);
        }
    }

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

    // Free the memory on success
    kfree(ptr2);

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Heap Allocate - Heavy (x1 mil)", kheapHeavyAllocateTest) {
    const size_t allocSize = 1000;
    const size_t oneMillion = 1000000;
    void** savedPointers = (void**)kmalloc(sizeof(void*) * allocSize);
    ASSERT_TRUE_CRITICAL(savedPointers, "Failed to allocate a buffer for memory pointers");

    // Perform a heavy-weight tight loop allocation test
    for (size_t i = 0; i < oneMillion; i++) {
        void* ptr = kmalloc(allocSize);
        ASSERT_TRUE_CRITICAL(ptr, "Allocated memory pointer was null");
        
        savedPointers[i] = ptr;
    }

    // Free all the allocated memory in this test
    for (size_t i = 0; i < oneMillion; i++) {
        kfree(savedPointers[i]);
    }

    // Free the pointer buffer
    kfree(savedPointers);

    return UNIT_TEST_SUCCESS;
}
