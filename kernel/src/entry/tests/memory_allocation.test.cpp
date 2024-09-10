#include "kernel_unit_tests.h"
#include <paging/page.h>
#include <memory/kmemory.h>
#include <paging/page.h>

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

            // Free the aligned memory pointer
            kfreeAligned(ptr);
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

DECLARE_UNIT_TEST("Heap Allocate - Heavy", kheapHeavyAllocateTest) {
    const size_t allocSize = 100000;  // Allocation size in bytes
    const size_t iterations = 8000; // Number of allocations
    const size_t reportInterval = iterations / 10; // Report every 10% of iterations
    const size_t bytesPerMB = 1024 * 1024;
    size_t nextMemoryMilestone = 100 * bytesPerMB; // First memory milestone (100 MB)

    void** savedPointers = (void**)kmalloc(sizeof(void*) * iterations);
    ASSERT_TRUE_CRITICAL(savedPointers, "Failed to allocate a buffer for memory pointers");

    size_t totalAllocatedBytes = 0;  // Track total allocated memory in bytes

    // Perform a heavy-weight tight loop allocation test
    for (size_t i = 0; i < iterations; i++) {
        void* ptr = kmalloc(allocSize);
        ASSERT_TRUE_CRITICAL(ptr, "Allocated memory pointer was null");
        
        savedPointers[i] = ptr;
        totalAllocatedBytes += allocSize;

        // Print progress every 10% of iterations or every 100 MB
        if ((i + 1) % reportInterval == 0 || totalAllocatedBytes >= nextMemoryMilestone) {
            kuPrint(UNIT_TEST "Allocated %llu MB of memory after %llu iterations\n",
                (unsigned long long)(totalAllocatedBytes / bytesPerMB),
                (unsigned long long)(i + 1));
            
            // Update the memory milestone for the next 100 MB
            nextMemoryMilestone += 100 * bytesPerMB;
        }
    }

    kuPrint(UNIT_TEST "Finished allocating %llu MB of memory in total\n", 
        (unsigned long long)(totalAllocatedBytes / bytesPerMB));

    // Free all the allocated memory in this test
    for (size_t i = 0; i < iterations; i++) {
        kfree(savedPointers[i]);
    }

    // Free the pointer buffer
    kfree(savedPointers);

    return UNIT_TEST_SUCCESS;
}
