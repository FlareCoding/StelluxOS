#include <unit_tests/unit_tests.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <memory/memory.h>

using namespace vmm;

// Helper function to verify that a given virtual address is mapped and returns a nonzero physical address
static bool is_mapped(void* vaddr) {
    uintptr_t paddr = paging::get_physical_address(vaddr);
    return (paddr != 0);
}

// Helper function to verify that a given virtual address maps to a specific physical address
static bool maps_to(void* vaddr, uintptr_t expected_paddr) {
    uintptr_t paddr = paging::get_physical_address(vaddr);
    return (paddr == expected_paddr);
}

// Test single page allocation
DECLARE_UNIT_TEST("vmm alloc_virtual_page", test_vmm_alloc_virtual_page) {
    void* vaddr = alloc_virtual_page(DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(vaddr != nullptr, "alloc_virtual_page should return a non-null pointer");
    ASSERT_TRUE(is_mapped(vaddr), "Returned page should be mapped to a physical page");

    // Unmap the page and verify it's unmapped
    unmap_virtual_page((uintptr_t)vaddr);
    ASSERT_FALSE(is_mapped(vaddr), "After unmapping, the page should no longer be mapped");

    return UNIT_TEST_SUCCESS;
}

// Test mapping a known physical page
DECLARE_UNIT_TEST("vmm map_physical_page", test_vmm_map_physical_page) {
    // We have a known good physical page at 0x12000
    void* vaddr = map_physical_page(0x12000, DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(vaddr != nullptr, "map_physical_page should return non-null pointer");
    ASSERT_TRUE(
        maps_to(vaddr, 0x12000),
        "Virtual address should map to physical 0x12000, mapped to 0x%llx instead",
        paging::get_physical_address(vaddr)
    );

    // Unmap and verify
    unmap_virtual_page((uintptr_t)vaddr);
    ASSERT_FALSE(is_mapped(vaddr), "After unmapping, no longer mapped");

    return UNIT_TEST_SUCCESS;
}

// Test allocating multiple pages
DECLARE_UNIT_TEST("vmm alloc_virtual_pages", test_vmm_alloc_virtual_pages) {
    size_t count = 3;
    void* base = alloc_virtual_pages(count, DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(base != nullptr, "alloc_virtual_pages should return a valid pointer");
    // Check each page in the range is mapped
    for (size_t i = 0; i < count; i++) {
        void* vaddr = (void*)((uintptr_t)base + i * 0x1000);
        ASSERT_TRUE(is_mapped(vaddr), "Each allocated page should be mapped");
    }

    // Unmap them
    unmap_contiguous_virtual_pages((uintptr_t)base, count);
    for (size_t i = 0; i < count; i++) {
        void* vaddr = (void*)((uintptr_t)base + i * 0x1000);
        ASSERT_FALSE(is_mapped(vaddr), "After unmapping, no page should remain mapped");
    }

    return UNIT_TEST_SUCCESS;
}

// Test allocating contiguous virtual pages
DECLARE_UNIT_TEST("vmm alloc_contiguous_virtual_pages", test_vmm_alloc_contiguous_virtual_pages) {
    size_t count = 4;
    void* base = alloc_contiguous_virtual_pages(count, DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(base != nullptr, "alloc_contiguous_virtual_pages should return a valid pointer");
    // Verify each is mapped and contiguous
    for (size_t i = 0; i < count; i++) {
        void* vaddr = (void*)((uintptr_t)base + i * 0x1000);
        ASSERT_TRUE(is_mapped(vaddr), "Each contiguous allocated page should be mapped");
    }

    // Unmap them
    unmap_contiguous_virtual_pages((uintptr_t)base, count);
    for (size_t i = 0; i < count; i++) {
        void* vaddr = (void*)((uintptr_t)base + i * 0x1000);
        ASSERT_FALSE(is_mapped(vaddr), "After unmapping contiguous pages, no page should remain mapped");
    }

    return UNIT_TEST_SUCCESS;
}

// Test mapping contiguous physical pages
DECLARE_UNIT_TEST("vmm map_contiguous_physical_pages", test_vmm_map_contiguous_physical_pages) {
    // We'll map two known physical pages: 0x12000 and 0x13000
    size_t count = 2;
    void* base = map_contiguous_physical_pages(0x12000, count, DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(base != nullptr, "map_contiguous_physical_pages should return a valid pointer");

    // Check the mapping
    void* vaddr1 = base;
    void* vaddr2 = (void*)((uintptr_t)base + 0x1000);
    ASSERT_TRUE(maps_to(vaddr1, 0x12000), "First page should map to physical 0x12000");
    ASSERT_TRUE(maps_to(vaddr2, 0x13000), "Second page should map to physical 0x13000");

    // Unmap and verify
    unmap_contiguous_virtual_pages((uintptr_t)base, count);
    ASSERT_FALSE(is_mapped(vaddr1), "After unmapping, no longer mapped");
    ASSERT_FALSE(is_mapped(vaddr2), "After unmapping, no longer mapped");

    return UNIT_TEST_SUCCESS;
}

// Test unmap_virtual_page explicitly after mapping a single page
DECLARE_UNIT_TEST("vmm unmap_virtual_page", test_vmm_unmap_virtual_page) {
    void* vaddr = alloc_virtual_page(DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(is_mapped(vaddr), "Should be mapped after alloc_virtual_page");

    unmap_virtual_page((uintptr_t)vaddr);
    ASSERT_FALSE(is_mapped(vaddr), "After unmap, should not be mapped");

    return UNIT_TEST_SUCCESS;
}

// Test unmap_contiguous_virtual_pages explicitly
DECLARE_UNIT_TEST("vmm unmap_contiguous_virtual_pages", test_vmm_unmap_contiguous_virtual_pages) {
    size_t count = 3;
    void* base = alloc_virtual_pages(count, DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(base != nullptr, "Should allocate a contiguous block of virtual pages");

    // Check mapping
    for (size_t i = 0; i < count; i++) {
        ASSERT_TRUE(is_mapped((void*)((uintptr_t)base + i * 0x1000)), "Page should be mapped");
    }

    // Unmap
    unmap_contiguous_virtual_pages((uintptr_t)base, count);
    for (size_t i = 0; i < count; i++) {
        ASSERT_FALSE(is_mapped((void*)((uintptr_t)base + i * 0x1000)), "Should be unmapped now");
    }

    return UNIT_TEST_SUCCESS;
}

// Test repeated allocations and deallocations to ensure stability
DECLARE_UNIT_TEST("vmm repeated allocations", test_vmm_repeated_allocations) {
    for (int i = 0; i < 5; i++) {
        void* page = alloc_virtual_page(DEFAULT_PRIV_PAGE_FLAGS);
        ASSERT_TRUE(page != nullptr, "alloc_virtual_page should succeed on iteration");
        ASSERT_TRUE(is_mapped(page), "Allocated page should be mapped");

        unmap_virtual_page((uintptr_t)page);
        ASSERT_FALSE(is_mapped(page), "Should be unmapped after unmap_virtual_page");
    }

    return UNIT_TEST_SUCCESS;
}

// Test allocating large contiguous ranges
DECLARE_UNIT_TEST("vmm large contiguous allocation", test_vmm_large_contiguous) {
    // Allocate a larger number of pages, say 8
    size_t count = 8;
    void* base = alloc_contiguous_virtual_pages(count, DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(base != nullptr, "alloc_contiguous_virtual_pages should succeed for larger count");

    for (size_t i = 0; i < count; i++) {
        void* vaddr = (void*)((uintptr_t)base + i * 0x1000);
        ASSERT_TRUE(is_mapped(vaddr), "Each page in large contiguous allocation should be mapped");
    }

    // Unmap them and verify
    unmap_contiguous_virtual_pages((uintptr_t)base, count);
    for (size_t i = 0; i < count; i++) {
        void* vaddr = (void*)((uintptr_t)base + i * 0x1000);
        ASSERT_FALSE(is_mapped(vaddr), "After unmapping large range, none should remain mapped");
    }

    return UNIT_TEST_SUCCESS;
}

// Test mapping an already allocated virtual page to a known physical address
DECLARE_UNIT_TEST("vmm remap to physical", test_vmm_remap_to_physical) {
    // First, allocate a virtual page
    void* vaddr = alloc_virtual_page(DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(is_mapped(vaddr), "Should be mapped initially");

    // Get its physical address
    uintptr_t old_paddr = paging::get_physical_address(vaddr);
    ASSERT_TRUE(old_paddr != 0, "Should have a valid physical address");

    // Now unmap it
    unmap_virtual_page((uintptr_t)vaddr);
    ASSERT_FALSE(is_mapped(vaddr), "Should be unmapped now");

    // Map it again to a known physical address
    void* new_vaddr = map_physical_page(0x13000, DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(new_vaddr != nullptr, "Should successfully map a known physical page");
    ASSERT_TRUE(maps_to(new_vaddr, 0x13000), "Should map to new physical address 0x13000");

    // Cleanup
    unmap_virtual_page((uintptr_t)new_vaddr);
    ASSERT_FALSE(is_mapped(new_vaddr), "Should be unmapped after cleanup");

    return UNIT_TEST_SUCCESS;
}

// Test heavy allocation exhaustion scenario
DECLARE_UNIT_TEST("vmm heavy allocation exhaustion", test_vmm_heavy_allocation_exhaustion) {
    const size_t max_attempts = 1000;
    void** allocated = (void**)zmalloc(max_attempts * sizeof(void*));
    ASSERT_TRUE(allocated != nullptr, "Should be able to allocate temporary array");

    size_t allocated_count = 0;
    size_t last_percentage = 0;

    serial::printf("[INFO] Starting heavy allocation test (up to %llu attempts)\n", max_attempts);
    serial::printf("[INFO] Allocation progress: 0%%"); // Initial progress

    for (size_t i = 0; i < max_attempts; i++) {
        size_t current_percentage = (i * 100) / max_attempts;
        if (current_percentage > last_percentage) {
            last_percentage = current_percentage;
            // Move cursor back to start of line and print updated percentage
            serial::printf("\r[INFO] Allocation progress: %llu%%", current_percentage);
        }

        void* page = alloc_virtual_page(DEFAULT_PRIV_PAGE_FLAGS);
        if (page == nullptr) {
            // Allocation failed, finalize progress line and break
            serial::printf("\r[INFO] Allocation progress: %llu%% - Exhausted at attempt %llu\n", current_percentage, i);
            break;
        } else {
            allocated[allocated_count++] = page;
        }
    }

    // If we never broke out with a failure, print final newline
    if (allocated_count == max_attempts) {
        serial::printf("\r[INFO] Allocation progress: 100%% - Reached max attempts without exhaustion\n");
    }

    serial::printf("[INFO] Allocated %llu pages before exhaustion.\n", allocated_count);

    // We expect to have allocated at least one page
    ASSERT_TRUE(allocated_count > 0, "Should have allocated at least one page");

    // Free all allocated pages
    for (size_t i = 0; i < allocated_count; i++) {
        unmap_virtual_page((uintptr_t)allocated[i]);
        ASSERT_FALSE((bool)(paging::get_physical_address(allocated[i])), "Page should be unmapped after freeing");
    }

    free(allocated);

    // Try allocating again after freeing all pages
    void* page = alloc_virtual_page(DEFAULT_PRIV_PAGE_FLAGS);
    ASSERT_TRUE(page != nullptr, "Should be able to allocate a page again after freeing");

    // Cleanup
    unmap_virtual_page((uintptr_t)page);

    return UNIT_TEST_SUCCESS;
}
