#include <unit_tests/unit_tests.h>
#include <memory/allocators/dma_allocator.h>
#include <memory/paging.h>
#include <memory/vmm.h>
#include <memory/memory.h>

// Use the allocators namespace for brevity
using namespace allocators;

// Helper function to check alignment
static bool is_aligned(uintptr_t addr, size_t alignment) {
    return (addr % alignment) == 0;
}

// Helper function to check boundary constraints
static bool does_not_cross_boundary(uintptr_t addr, size_t size, size_t boundary) {
    return (addr / boundary) == ((addr + size - 1) / boundary);
}

// Test DMA allocation with default alignment and boundary
DECLARE_UNIT_TEST("dma_allocator default allocation", test_dma_allocator_default_allocation) {
    size_t size = 4096; // Default alignment is 4096
    void* ptr = dma_allocator::get().allocate(size);
    uintptr_t phys_addr = paging::get_physical_address(ptr);

    ASSERT_TRUE(ptr != nullptr, "DMA allocation should return a non-null pointer, got: 0x%llx", (uintptr_t)ptr);
    ASSERT_TRUE(phys_addr != 0, "Allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr);
    ASSERT_TRUE(is_aligned(phys_addr, 4096), "Physical address 0x%llx is not aligned to 4096 bytes", phys_addr);

    // Cleanup
    dma_allocator::get().free(ptr);

    return UNIT_TEST_SUCCESS;
}

// Test DMA allocation with custom alignment
DECLARE_UNIT_TEST("dma_allocator custom alignment", test_dma_allocator_custom_alignment) {
    size_t size = 8192;
    size_t alignment = 8192;
    void* ptr = dma_allocator::get().allocate(size, alignment);
    uintptr_t phys_addr = paging::get_physical_address(ptr);

    ASSERT_TRUE(ptr != nullptr, "DMA allocation with alignment %llu should return a non-null pointer, got: 0x%llx", alignment, (uintptr_t)ptr);
    ASSERT_TRUE(phys_addr != 0, "Allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr);
    ASSERT_TRUE(is_aligned(phys_addr, alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addr, alignment);

    // Cleanup
    dma_allocator::get().free(ptr);

    return UNIT_TEST_SUCCESS;
}

// Test DMA allocation with boundary constraints
DECLARE_UNIT_TEST("dma_allocator boundary constraints", test_dma_allocator_boundary_constraints) {
    size_t size = 4096;
    size_t alignment = 4096;
    size_t boundary = 65536;

    void* ptr = dma_allocator::get().allocate(size, alignment, boundary);
    uintptr_t phys_addr = paging::get_physical_address(ptr);

    ASSERT_TRUE(ptr != nullptr, "DMA allocation with boundary %llu should return a non-null pointer, got: 0x%llx", boundary, (uintptr_t)ptr);
    ASSERT_TRUE(phys_addr != 0, "Allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr);
    ASSERT_TRUE(is_aligned(phys_addr, alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addr, alignment);
    ASSERT_TRUE(does_not_cross_boundary(phys_addr, size, boundary), "Physical address 0x%llx with size %llu crosses boundary %llu", phys_addr, size, boundary);

    // Cleanup
    dma_allocator::get().free(ptr);

    return UNIT_TEST_SUCCESS;
}

// Test multiple DMA allocations ensuring unique and correctly aligned physical addresses
DECLARE_UNIT_TEST("dma_allocator multiple allocations", test_dma_allocator_multiple_allocations) {
    size_t size = 4096;
    size_t alignment = 4096;
    size_t num_allocs = 10;
    void* ptrs[num_allocs];
    uintptr_t phys_addrs[num_allocs];

    // Allocate multiple blocks
    for (size_t i = 0; i < num_allocs; i++) {
        ptrs[i] = dma_allocator::get().allocate(size, alignment);
        phys_addrs[i] = paging::get_physical_address(ptrs[i]);

        ASSERT_TRUE(ptrs[i] != nullptr, "DMA allocation %llu should return a non-null pointer, got: 0x%llx", i, (uintptr_t)ptrs[i]);
        ASSERT_TRUE(phys_addrs[i] != 0, "Allocated DMA memory %llu should have a valid physical address, address was: 0x%llx", i, phys_addrs[i]);
        ASSERT_TRUE(is_aligned(phys_addrs[i], alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addrs[i], alignment);

        // Ensure uniqueness
        for (size_t j = 0; j < i; j++) {
            ASSERT_TRUE(phys_addrs[i] != phys_addrs[j], "Physical address 0x%llx should be unique, duplicates found with 0x%llx", phys_addrs[i], phys_addrs[j]);
        }
    }

    // Cleanup
    for (size_t i = 0; i < num_allocs; i++) {
        dma_allocator::get().free(ptrs[i]);
    }

    return UNIT_TEST_SUCCESS;
}

// Test DMA allocation and freeing
DECLARE_UNIT_TEST("dma_allocator allocate and free", test_dma_allocator_allocate_and_free) {
    size_t size = 4096;
    size_t alignment = 4096;

    // Allocate a block
    void* ptr = dma_allocator::get().allocate(size, alignment);
    uintptr_t phys_addr = paging::get_physical_address(ptr);

    ASSERT_TRUE(ptr != nullptr, "DMA allocation should return a non-null pointer, got: 0x%llx", (uintptr_t)ptr);
    ASSERT_TRUE(phys_addr != 0, "Allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr);
    ASSERT_TRUE(is_aligned(phys_addr, alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addr, alignment);

    // Free the block
    dma_allocator::get().free(ptr);

    // Attempt to allocate again, potentially getting the same address
    void* ptr2 = dma_allocator::get().allocate(size, alignment);
    uintptr_t phys_addr2 = paging::get_physical_address(ptr2);

    ASSERT_TRUE(ptr2 != nullptr, "Re-allocation should return a non-null pointer, got: 0x%llx", (uintptr_t)ptr2);
    ASSERT_TRUE(phys_addr2 != 0, "Re-allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr2);
    ASSERT_TRUE(is_aligned(phys_addr2, alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addr2, alignment);

    // It's possible to get the same address if the allocator reuses freed blocks
    // So, we won't assert that phys_addr == phys_addr2

    // Cleanup
    dma_allocator::get().free(ptr2);

    return UNIT_TEST_SUCCESS;
}

// Test DMA allocation with different boundary sizes
DECLARE_UNIT_TEST("dma_allocator various boundaries", test_dma_allocator_various_boundaries) {
    size_t size = 4096;
    size_t alignment = 4096;
    size_t boundaries[] = { 4096, 8192, 16384, 32768, 65536 };
    size_t num_boundaries = sizeof(boundaries) / sizeof(boundaries[0]);

    for (size_t i = 0; i < num_boundaries; i++) {
        size_t boundary = boundaries[i];
        void* ptr = dma_allocator::get().allocate(size, alignment, boundary);
        uintptr_t phys_addr = paging::get_physical_address(ptr);

        ASSERT_TRUE(ptr != nullptr, "DMA allocation with boundary %llu should return a non-null pointer, got: 0x%llx", boundary, (uintptr_t)ptr);
        ASSERT_TRUE(phys_addr != 0, "Allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr);
        ASSERT_TRUE(is_aligned(phys_addr, alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addr, alignment);
        ASSERT_TRUE(does_not_cross_boundary(phys_addr, size, boundary), "Physical address 0x%llx with size %llu crosses boundary %llu", phys_addr, size, boundary);

        // Cleanup
        dma_allocator::get().free(ptr);
    }

    return UNIT_TEST_SUCCESS;
}

// Test DMA allocator multiple allocations and frees
DECLARE_UNIT_TEST("dma_allocator allocate and free multiple times", test_dma_allocator_allocate_free_multiple) {
    size_t size = 4096;
    size_t alignment = 4096;
    size_t num_iterations = 100;

    void* ptrs[num_iterations];
    uintptr_t phys_addrs[num_iterations];

    for (size_t i = 0; i < num_iterations; i++) {
        ptrs[i] = dma_allocator::get().allocate(size, alignment);
        phys_addrs[i] = paging::get_physical_address(ptrs[i]);

        ASSERT_TRUE(ptrs[i] != nullptr, "DMA allocation %llu should return a non-null pointer, got: 0x%llx", i, (uintptr_t)ptrs[i]);
        ASSERT_TRUE(phys_addrs[i] != 0, "Allocated DMA memory %llu should have a valid physical address, address was: 0x%llx", i, phys_addrs[i]);
        ASSERT_TRUE(is_aligned(phys_addrs[i], alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addrs[i], alignment);
    }

    // Free all allocations
    for (size_t i = 0; i < num_iterations; i++) {
        dma_allocator::get().free(ptrs[i]);
    }

    // Reallocate again to ensure memory is reusable
    for (size_t i = 0; i < num_iterations; i++) {
        ptrs[i] = dma_allocator::get().allocate(size, alignment);
        phys_addrs[i] = paging::get_physical_address(ptrs[i]);

        ASSERT_TRUE(ptrs[i] != nullptr, "Re-allocation %llu should return a non-null pointer, got: 0x%llx", i, (uintptr_t)ptrs[i]);
        ASSERT_TRUE(phys_addrs[i] != 0, "Re-allocated DMA memory %llu should have a valid physical address, address was: 0x%llx", i, phys_addrs[i]);
        ASSERT_TRUE(is_aligned(phys_addrs[i], alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addrs[i], alignment);
    }

    // Cleanup
    for (size_t i = 0; i < num_iterations; i++) {
        dma_allocator::get().free(ptrs[i]);
    }

    return UNIT_TEST_SUCCESS;
}

// Test DMA allocator alignment greater than block size
DECLARE_UNIT_TEST("dma_allocator alignment greater than block size", test_dma_allocator_large_alignment) {
    size_t size = 4096;
    size_t alignment = 32768; // Greater than typical block size

    void* ptr = dma_allocator::get().allocate(size, alignment);
    uintptr_t phys_addr = paging::get_physical_address(ptr);

    ASSERT_TRUE(ptr != nullptr, "DMA allocation with large alignment %llu should return a non-null pointer, got: 0x%llx", alignment, (uintptr_t)ptr);
    ASSERT_TRUE(phys_addr != 0, "Allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr);
    ASSERT_TRUE(is_aligned(phys_addr, alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addr, alignment);
    // Assuming boundary is default, 65536
    ASSERT_TRUE(does_not_cross_boundary(phys_addr, size, 65536), "Physical address 0x%llx with size %llu crosses boundary 65536", phys_addr, size);

    // Cleanup
    dma_allocator::get().free(ptr);

    return UNIT_TEST_SUCCESS;
}

// Test DMA allocator allocation with maximum boundary and alignment
DECLARE_UNIT_TEST("dma_allocator maximum alignment and boundary", test_dma_allocator_max_alignment_boundary) {
    size_t size = 65536; // Size equal to boundary
    size_t alignment = 65536; // Alignment equal to boundary
    size_t boundary = 65536;

    void* ptr = dma_allocator::get().allocate(size, alignment, boundary);
    uintptr_t phys_addr = paging::get_physical_address(ptr);

    ASSERT_TRUE(ptr != nullptr, "DMA allocation with maximum alignment %llu should return a non-null pointer, got: 0x%llx", alignment, (uintptr_t)ptr);
    ASSERT_TRUE(phys_addr != 0, "Allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr);
    ASSERT_TRUE(is_aligned(phys_addr, alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addr, alignment);
    ASSERT_TRUE(does_not_cross_boundary(phys_addr, size, boundary), "Physical address 0x%llx with size %llu crosses boundary %llu", phys_addr, size, boundary);

    // Cleanup
    dma_allocator::get().free(ptr);

    return UNIT_TEST_SUCCESS;
}

// Test DMA allocator allocate with large boundary and alignment
DECLARE_UNIT_TEST("dma_allocator large boundary and alignment", test_dma_allocator_large_boundary_alignment) {
    size_t size = 65536;        // 64KB
    size_t alignment = 65536;   // 64KB
    size_t boundary = 262144;   // 256KB

    void* ptr = dma_allocator::get().allocate(size, alignment, boundary);
    uintptr_t phys_addr = paging::get_physical_address(ptr);

    ASSERT_TRUE(ptr != nullptr, "DMA allocation with large alignment %llu should return a non-null pointer, got: 0x%llx", alignment, (uintptr_t)ptr);
    ASSERT_TRUE(phys_addr != 0, "Allocated DMA memory should have a valid physical address, address was: 0x%llx", phys_addr);
    ASSERT_TRUE(is_aligned(phys_addr, alignment), "Physical address 0x%llx is not aligned to %llu bytes", phys_addr, alignment);
    ASSERT_TRUE(does_not_cross_boundary(phys_addr, size, boundary), "Physical address 0x%llx with size %llu crosses boundary %llu", phys_addr, size, boundary);

    // Cleanup
    dma_allocator::get().free(ptr);

    return UNIT_TEST_SUCCESS;
}
