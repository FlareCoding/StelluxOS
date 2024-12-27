#ifndef DMA_ALLOCATOR_H
#define DMA_ALLOCATOR_H
#include <sync.h>
#include <kstl/vector.h>

namespace allocators {
class dma_allocator {
public:
    // Represents a pool of pre-allocated DMA memory
    struct dma_pool {
        size_t block_size;       // Size of each block in this pool
        size_t alignment;        // Alignment requirement
        size_t max_blocks;       // Maximum number of blocks in this pool
        uintptr_t phys_base;     // Physical base address for the pool
        uintptr_t virt_base;     // Virtual base address for the pool
        size_t used_block_count; // Number of blocks currently in use
        uint64_t* used_blocks;   // Bitmap tracking used blocks
    };

    // Singleton access
    __PRIVILEGED_CODE static dma_allocator& get();

    // Initialization
    __PRIVILEGED_CODE void init();

    // DMA Memory Allocation
    __PRIVILEGED_CODE void* allocate(size_t size, size_t alignment = 4096, size_t boundary = 65536);
    __PRIVILEGED_CODE void free(void* ptr);

    // Pool Management
    __PRIVILEGED_CODE void create_pool(size_t block_size, size_t alignment, size_t max_blocks);
    __PRIVILEGED_CODE void debug_dma();

private:
    mutex m_lock = mutex();
    kstl::vector<dma_pool> m_pools;
};

} // namespace allocators

#endif // DMA_ALLOCATOR_H
