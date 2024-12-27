#include <memory/allocators/dma_allocator.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <serial/serial.h>

namespace allocators {
__PRIVILEGED_CODE
dma_allocator& dma_allocator::get() {
    GENERATE_STATIC_SINGLETON(dma_allocator);
}

void dma_allocator::init() {
    // Clear any existing pools
    m_pools.clear();

    // Create default pools
    create_pool(64, 64, 16384);             // Pool of 1638 64 byte blocks (1MB total)
    create_pool(256, 256, 4096);            // Pool of 4096 256 byte blocks (1MB total)
    create_pool(1024, 1024, 2048);          // Pool of 2048 1KB blocks (2MB total)
    create_pool(4096, 4096, 1024);          // Pool of 1024 4KB blocks (4MB total)
    create_pool(64 * 1024, 64 * 1024, 64);  // Pool of 64 64KB blocks (4MB total)
}

void dma_allocator::create_pool(size_t block_size, size_t alignment, size_t max_blocks) {
    mutex_guard guard(m_lock);

    // Ensure alignment is at least as large as the block size
    if (alignment < block_size) {
        alignment = block_size;
    }

    // Calculate the total size required for the pool
    size_t pool_size = block_size * max_blocks;

    // Calculate the number of extra pages needed for alignment
    size_t alignment_pages = alignment / PAGE_SIZE;

    // Total pages to allocate: pool_size + alignment
    size_t total_pages = (pool_size / PAGE_SIZE) + alignment_pages;

    // Allocate the required number of contiguous virtual pages
    void* virt_base_alloc = vmm::alloc_contiguous_virtual_pages(total_pages, PTE_DEFAULT_UNPRIV_KERNEL_FLAGS | PTE_PCD);
    if (!virt_base_alloc) {
        // Handle allocation failure
        serial::printf("DMA pool creation failed. Failed to allocate %llu pages\n", total_pages);
        return;
    }

    // Retrieve the physical base address of the allocated pages
    uintptr_t phys_base_alloc = paging::get_physical_address(virt_base_alloc);
    if (!phys_base_alloc) {
        // Clean up virtual memory and fail
        vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(virt_base_alloc), total_pages);
        serial::printf("DMA pool creation failed: Invalid physical address\n");
        return;
    }

    // Find the first aligned phys_base within the allocated range
    uintptr_t aligned_phys_base = phys_base_alloc;
    if (aligned_phys_base % alignment != 0) {
        aligned_phys_base += alignment - (aligned_phys_base % alignment);
        // Ensure that aligned_phys_base + pool_size does not exceed allocated range
        if (aligned_phys_base + pool_size > phys_base_alloc + pool_size + alignment) {
            // Cannot satisfy alignment within allocated range
            vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(virt_base_alloc), total_pages);
            serial::printf("DMA pool creation failed: phys_base alignment requirement not met\n");
            return;
        }
    }

    // Calculate virt_base aligned
    uintptr_t virt_base = reinterpret_cast<uintptr_t>(virt_base_alloc);
    if (virt_base % alignment != 0) {
        virt_base += alignment - (virt_base % alignment);
        // Ensure that virt_base + pool_size does not exceed allocated virtual range
        if (virt_base + pool_size > reinterpret_cast<uintptr_t>(virt_base_alloc) + pool_size + alignment) {
            vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(virt_base_alloc), total_pages);
            serial::printf("DMA pool creation failed: virt_base alignment requirement not met\n");
            return;
        }
    }

    // Initialize the pool metadata
    dma_pool new_pool = {
        .block_size = block_size,
        .alignment = alignment,
        .max_blocks = max_blocks,
        .phys_base = aligned_phys_base,
        .virt_base = virt_base,
        .used_block_count = 0,
        .used_blocks = new uint64_t[(max_blocks + 63) / 64]() // Initialize bitmap to 0
    };

    // Add the new pool to the list
    m_pools.push_back(new_pool);

    // Log the creation
    serial::printf(
        "DMA pool created: block_size = 0x%llx, alignment = 0x%llx, "
        "max_blocks = %llu, phys_base = 0x%llx, virt_base = 0x%llx\n",
        block_size, alignment, max_blocks, aligned_phys_base, virt_base
    );
}

void* dma_allocator::allocate(size_t size, size_t alignment, size_t boundary) {
    mutex_guard guard(m_lock);

    if (alignment < size) {
        alignment = size;
    }

    // Iterate over pools in reverse order to prioritize newer pools
    for (size_t i = m_pools.size(); i-- > 0;) {
        dma_pool& pool = m_pools[i];
        if (size > pool.block_size || alignment > pool.alignment) {
            continue;
        }

        for (size_t j = 0; j < pool.max_blocks; ++j) {
            size_t bitmap_index = j / 64;
            size_t bit_index = j % 64;

            if ((pool.used_blocks[bitmap_index] & (1ULL << bit_index)) == 0) {
                uintptr_t block_phys = pool.phys_base + j * pool.block_size;
                uintptr_t block_virt = pool.virt_base + j * pool.block_size;

                if (block_phys % alignment == 0 &&
                    (block_phys / boundary == (block_phys + size - 1) / boundary)) {
                    pool.used_blocks[bitmap_index] |= (1ULL << bit_index);
                    ++pool.used_block_count;

                    return reinterpret_cast<void*>(block_virt);
                }
            }
        }
    }

    serial::printf("[*] DMA allocation failed: size = 0x%llx, alignment = 0x%llx, boundary = 0x%llx\n", size, alignment, boundary);
    return nullptr;
}

void dma_allocator::free(void* ptr) {
    mutex_guard guard(m_lock);

    uintptr_t virt_addr = reinterpret_cast<uintptr_t>(ptr);

    for (dma_pool& pool : m_pools) {
        if (virt_addr >= pool.virt_base && virt_addr < pool.virt_base + pool.block_size * pool.max_blocks) {
            size_t block_index = (virt_addr - pool.virt_base) / pool.block_size;
            size_t bitmap_index = block_index / 64;
            size_t bit_index = block_index % 64;

            if (pool.used_blocks[bitmap_index] & (1ULL << bit_index)) {
                pool.used_blocks[bitmap_index] &= ~(1ULL << bit_index);
                --pool.used_block_count;

                return;
            }

            serial::printf("DMA Free failed: Block #%llu is already free\n", block_index);
            return;
        }
    }

    serial::printf("[*] DMA Free failed: invalid address 0x%llx\n", virt_addr);
}

void dma_allocator::debug_dma() {
    mutex_guard guard(m_lock);

    serial::printf("DMA Pools:\n");
    for (size_t i = 0; i < m_pools.size(); ++i) {
        const dma_pool& pool = m_pools[i];
        serial::printf(
            "  Pool #%llu: block_size = 0x%llx, alignment = 0x%llx,"
            "max_blocks = %llu, used_blocks = %llu, phys_base = 0x%llx, virt_base = 0x%llx\n",
            i, pool.block_size, pool.alignment, pool.max_blocks, pool.used_block_count,
            pool.phys_base, pool.virt_base
        );
    }
    serial::printf("\n");
}
} // namespace allocators
