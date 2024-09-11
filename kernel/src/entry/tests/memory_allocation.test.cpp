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

DECLARE_UNIT_TEST("Heap Allocate - Heavy", kheapHeavyAllocateUnitTest) {
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
                (uint64_t)(totalAllocatedBytes / bytesPerMB),
                (uint64_t)(i + 1));
            
            // Update the memory milestone for the next 100 MB
            nextMemoryMilestone += 100 * bytesPerMB;
        }
    }

    kuPrint(UNIT_TEST "Finished allocating %llu MB of memory in total\n", 
        (uint64_t)(totalAllocatedBytes / bytesPerMB));

    // Free all the allocated memory in this test
    for (size_t i = 0; i < iterations; i++) {
        kfree(savedPointers[i]);
    }

    // Free the pointer buffer
    kfree(savedPointers);

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Heap Reallocate Test", kheapReallocateUnitTest) {
    // Allocate an initial block of memory
    void* ptr = kmalloc(ALLOC_SIZE);
    ASSERT_TRUE_CRITICAL(ptr, "Initial allocated memory pointer was null");

    // Reallocate the memory to a larger size
    void* new_ptr = krealloc(ptr, ALLOC_SIZE * 2);
    ASSERT_TRUE_CRITICAL(new_ptr, "Reallocated memory pointer was null");

    // Ensure the new pointer is valid and that the data wasn't corrupted (if possible to check)
    ASSERT_TRUE(ptr != new_ptr, "Reallocated memory pointer didn't change as expected when resizing");
    
    kuPrint(UNIT_TEST "Reallocated memory from %llu bytes to %llu bytes: Success\n", (uint64_t)ALLOC_SIZE, (uint64_t)(ALLOC_SIZE * 2));

    // Free the reallocated memory
    kfree(new_ptr);

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Paging - Allocate Single Page", pagingAllocateUnitTest) {
    auto& allocator = paging::getGlobalPageFrameAllocator();
    size_t usedMemoryBefore = allocator.getUsedSystemMemory(); // KB
    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryBefore);

    // Allocate a single page
    void* page = allocPage();

    // Measure system memory usage
    size_t usedMemoryAfter = allocator.getUsedSystemMemory();
    size_t memoryUsageChange = usedMemoryAfter - usedMemoryBefore;

    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryAfter);
    kuPrint(UNIT_TEST "Memory usage change: %lli bytes\n", memoryUsageChange);

    // Assert the system memory usage
    ASSERT_EQ(memoryUsageChange, PAGE_SIZE, "Incorrect system memory usage after page allocation");

    // Free the page
    freePage(page);

    usedMemoryAfter = allocator.getUsedSystemMemory();
    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryAfter);

    // The net change in used memory should be zero after freeing the page
    ASSERT_EQ(usedMemoryBefore, usedMemoryAfter, "Failed to properly free the allocated page memory");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Paging - Allocate Single Zeroed Page", pagingAllocateZeroedUnitTest) {
    auto& allocator = paging::getGlobalPageFrameAllocator();
    size_t usedMemoryBefore = allocator.getUsedSystemMemory(); // KB
    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryBefore);

    // Allocate a single page
    void* page = zallocPage();
    ASSERT_TRUE_CRITICAL(page, "Allocated memory pointer was null");

    // Verify that the page is properly zeroed out
    for (int i = 0; i < PAGE_SIZE; i++) {
        uint8_t* ptr = (uint8_t*)page;
        ASSERT_EQ(ptr[i], 0, "Allocated page memory contents were not zeroed out");
    }

    // Measure system memory usage
    size_t usedMemoryAfter = allocator.getUsedSystemMemory();
    size_t memoryUsageChange = usedMemoryAfter - usedMemoryBefore;

    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryAfter);
    kuPrint(UNIT_TEST "Memory usage change: %lli bytes\n", memoryUsageChange);

    // Assert the system memory usage
    ASSERT_EQ(memoryUsageChange, PAGE_SIZE, "Incorrect system memory usage after page allocation");

    // Free the page
    freePage(page);

    usedMemoryAfter = allocator.getUsedSystemMemory();
    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryAfter);

    // The net change in used memory should be zero after freeing the page
    ASSERT_EQ(usedMemoryBefore, usedMemoryAfter, "Failed to properly free the allocated page memory");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Paging - Allocate Multiple Zeroed Pages", pagingAllocateMultiplePagesUnitTest) {
    const size_t allocPageCount = 100;

    auto& allocator = paging::getGlobalPageFrameAllocator();
    size_t usedMemoryBefore = allocator.getUsedSystemMemory(); // KB
    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryBefore);

    // Allocate a single page
    void* ptr = zallocPages(100);
    ASSERT_TRUE_CRITICAL(ptr, "Allocated memory pointer was null");

    // Measure system memory usage
    size_t usedMemoryAfter = allocator.getUsedSystemMemory();
    size_t memoryUsageChange = usedMemoryAfter - usedMemoryBefore;

    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryAfter);
    kuPrint(UNIT_TEST "Memory usage change: %lli KB\n", memoryUsageChange / 1024);

    // Assert the system memory usage
    ASSERT_EQ(memoryUsageChange, PAGE_SIZE * allocPageCount, "Incorrect system memory usage after page allocation");

    // Free the page
    freePages(ptr, allocPageCount);

    usedMemoryAfter = allocator.getUsedSystemMemory();
    kuPrint(UNIT_TEST "System memory used: %lli KB\n", usedMemoryAfter);

    // The net change in used memory should be zero after freeing the page
    ASSERT_EQ(usedMemoryBefore, usedMemoryAfter, "Failed to properly free the allocated page memory");

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Paging and Heap Allocation Combined Test", kheapWithPagingAllocationUnitTest) {
    const size_t allocSize = 100000;  // Allocation size in bytes
    const size_t iterations = 8000; // Number of allocations
    const size_t reportInterval = iterations / 10; // Report every 10% of iterations
    const size_t bytesPerMB = 1024 * 1024;
    size_t nextMemoryMilestone = 100 * bytesPerMB; // First memory milestone (100 MB)

    void** savedHeapPointers = (void**)kmalloc(sizeof(void*) * iterations);
    ASSERT_TRUE_CRITICAL(savedHeapPointers, "Failed to allocate a buffer for heap memory pointers");

    void** savedPagePointers = (void**)kmalloc(sizeof(void*) * iterations);
    ASSERT_TRUE_CRITICAL(savedPagePointers, "Failed to allocate a buffer for allocated page pointers");

    size_t totalAllocatedBytes = 0;  // Track total allocated memory in bytes

    // Perform a heavy-weight tight loop allocation test
    for (size_t i = 0; i < iterations; i++) {
        void* ptr = kmalloc(allocSize);
        ASSERT_TRUE_CRITICAL(ptr, "Allocated heap memory pointer was null");

        void* page = zallocPage();
        ASSERT_TRUE_CRITICAL(page, "Allocated virtual page pointer was null");
        
        savedHeapPointers[i] = ptr;
        savedPagePointers[i] = page;

        totalAllocatedBytes += allocSize;

        // Print progress every 10% of iterations or every 100 MB
        if ((i + 1) % reportInterval == 0 || totalAllocatedBytes >= nextMemoryMilestone) {
            kuPrint(UNIT_TEST "Allocated %llu MB of heap memory and %llu pages after %llu iterations\n",
                (uint64_t)(totalAllocatedBytes / bytesPerMB),
                (uint64_t)(i + 1),
                (uint64_t)(i + 1));
            
            // Update the memory milestone for the next 100 MB
            nextMemoryMilestone += 100 * bytesPerMB;
        }
    }

    kuPrint(UNIT_TEST "Finished allocating %llu MB of heap memory and %llu pages in total\n", 
        (uint64_t)(totalAllocatedBytes / bytesPerMB), iterations);

    // Free all the allocated heap memory in this test
    for (size_t i = 0; i < iterations; i++) {
        kfree(savedHeapPointers[i]);
    }

    // Free all the allocated pages in this test
    for (size_t i = 0; i < iterations; i++) {
        freePage(savedPagePointers[i]);
    }

    // Free the pointer buffers
    kfree(savedHeapPointers);
    kfree(savedPagePointers);

    return UNIT_TEST_SUCCESS;
}
