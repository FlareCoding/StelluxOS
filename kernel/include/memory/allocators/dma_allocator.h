#ifndef DMA_ALLOCATOR_H
#define DMA_ALLOCATOR_H
#include <sync.h>
#include <kstl/vector.h>

namespace allocators {
/**
 * @class dma_allocator
 * @brief Manages allocation of Direct Memory Access (DMA) memory.
 * 
 * Provides mechanisms for allocating, freeing, and managing DMA-compatible memory,
 * ensuring alignment and boundary constraints are met.
 */
class dma_allocator {
public:
    /**
     * @struct dma_pool
     * @brief Represents a pool of pre-allocated DMA memory blocks.
     * 
     * Contains metadata about a pool of memory blocks, including alignment, size, and usage tracking.
     */
    struct dma_pool {
        size_t block_size;       // Size of each block in this pool
        size_t alignment;        // Alignment requirement
        size_t max_blocks;       // Maximum number of blocks in this pool
        uintptr_t phys_base;     // Physical base address for the pool
        uintptr_t virt_base;     // Virtual base address for the pool
        size_t used_block_count; // Number of blocks currently in use
        uint64_t* used_blocks;   // Bitmap tracking used blocks
    };

    /**
     * @brief Retrieves the singleton instance of the DMA allocator.
     * @return Reference to the singleton instance of the `dma_allocator`.
     * 
     * Provides a globally accessible instance of the allocator.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static dma_allocator& get();

    /**
     * @brief Initializes the DMA allocator.
     * 
     * Prepares the DMA allocator for use, including any necessary setup or configuration.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void init();

    /**
     * @brief Allocates DMA-compatible memory.
     * @param size The size of the memory block to allocate, in bytes.
     * @param alignment The alignment requirement for the memory block (default: 4096 bytes).
     * @param boundary The boundary limit for the memory block (default: 65536 bytes).
     * @return Pointer to the allocated memory, or `nullptr` if allocation fails.
     * 
     * Allocates a block of memory that satisfies the size, alignment, and boundary requirements,
     * ensuring compatibility with DMA operations.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* allocate(size_t size, size_t alignment = 4096, size_t boundary = 65536);

    /**
     * @brief Frees a previously allocated DMA memory block.
     * @param ptr Pointer to the memory block to free.
     * 
     * Releases the specified memory block, making it available for future allocations.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void free(void* ptr);

    /**
     * @brief Creates a new pool of DMA memory.
     * @param block_size The size of each block in the pool, in bytes.
     * @param alignment The alignment requirement for blocks in the pool.
     * @param max_blocks The maximum number of blocks in the pool.
     * 
     * Initializes a pool of memory blocks with the specified size, alignment, and capacity,
     * for efficient allocation and deallocation.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void create_pool(size_t block_size, size_t alignment, size_t max_blocks);

    /**
     * @brief Outputs debug information about the DMA allocator.
     * 
     * Prints or logs details about the current state of the DMA pools, including usage statistics
     * and allocation information, for debugging purposes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void debug_dma();

private:
    mutex m_lock = mutex();          /** Mutex for synchronizing access to DMA pools */
    kstl::vector<dma_pool> m_pools;  /** Vector of DMA memory pools */
};
} // namespace allocators

#endif // DMA_ALLOCATOR_H
