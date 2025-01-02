#ifndef HEAP_ALLOCATOR_H
#define HEAP_ALLOCATOR_H
#include <sync.h>

#define KERNEL_HEAP_INIT_SIZE               0x12C00000 // 300MB Kernel Heap

#define KERNEL_HEAP_SEGMENT_HDR_SIGNATURE   "HEAPHDR"

namespace allocators {
/**
 * @struct heap_segment_header
 * @brief Represents a header for a memory segment in the heap.
 * 
 * Each segment of the heap is preceded by this header, which stores metadata about
 * the segment, such as its size and whether it is free or allocated.
 */
struct heap_segment_header {
    uint8_t     magic[7];
    
    struct {
        uint8_t free        : 1;
        uint8_t reserved    : 7;
    } flags;

    uint64_t    size;

    heap_segment_header* next;
    heap_segment_header* prev;
} __attribute__((packed, aligned(16)));

/**
 * @class heap_allocator
 * @brief Manages dynamic memory allocation within a heap.
 * 
 * Provides methods for allocating, freeing, and managing memory within a predefined heap region.
 * Includes debugging and corruption detection capabilities.
 */
class heap_allocator {
public:
    /**
     * @brief Retrieves the singleton instance of the heap allocator.
     * @return Reference to the singleton instance of the `heap_allocator`.
     */
    static heap_allocator& get();

    /**
     * @brief Initializes the heap allocator with a specified memory region.
     * @param virt_base The virtual base address of the heap.
     * @param size The total size of the heap, in bytes.
     * 
     * Sets up the heap allocator with the given memory range, preparing it for allocation requests.
     */
    void init(uintptr_t virt_base, size_t size);

    /**
     * @brief Retrieves the base address of the heap.
     * @return Pointer to the first segment of the heap.
     * 
     * Provides access to the starting point of the heap for inspection or debugging purposes.
     */
    inline void* get_heap_base() const { return static_cast<void*>(m_first_segment); }

    /**
     * @brief Allocates memory from the heap.
     * @param size The size of the memory block to allocate, in bytes.
     * @return Pointer to the allocated memory block, or `nullptr` if allocation fails.
     * 
     * Finds a suitable free segment or expands the heap to accommodate the requested size.
     */
    void* allocate(size_t size);

    /**
     * @brief Frees a previously allocated memory block.
     * @param ptr Pointer to the memory block to free.
     * 
     * Marks the corresponding segment as free and attempts to merge adjacent free segments.
     */
    void free(void* ptr);

    /**
     * @brief Reallocates an existing memory block to a new size.
     * @param ptr Pointer to the memory block to resize.
     * @param new_size The new size of the memory block, in bytes.
     * @return Pointer to the resized memory block, or `nullptr` if resizing fails.
     * 
     * Attempts to resize the block in place or allocates a new block and copies the data.
     */
    void* reallocate(void* ptr, size_t new_size);

    /**
     * @brief Outputs debug information about the heap.
     * 
     * Logs details about the current state of the heap, including segment information and usage statistics.
     */
    void debug_heap();

    /**
     * @brief Outputs debug information about a specific heap segment.
     * @param ptr Pointer to a heap segment.
     * @param seg_id Optional identifier for the segment (-1 for default behavior).
     * 
     * Logs metadata and state of the specified segment for debugging purposes.
     */
    void debug_heap_segment(void* ptr, int64_t seg_id = -1);

    /**
     * @brief Outputs debug information for a user-provided heap pointer.
     * @param ptr Pointer to a user-allocated memory block.
     * @param id Optional identifier for the pointer (-1 for default behavior).
     * 
     * Logs details about the block corresponding to the pointer, such as size and location.
     */
    void debug_user_heap_pointer(void* ptr, int64_t id = -1);

    /**
     * @brief Detects corruption in the heap.
     * @param dbg_log Whether to log debug information if corruption is detected (default: true).
     * @return True if corruption is detected, false otherwise.
     * 
     * Scans the heap for invalid or inconsistent segment headers and optionally logs details.
     */
    bool detect_heap_corruption(bool dbg_log = true);

private:
    uint64_t                m_heap_size;
    heap_segment_header*    m_first_segment;

    // Call the constructor to ensure full object
    // construction including the base class.
    mutex                   m_heap_lock = mutex();

private:
    /**
     * @brief Allocates memory from the heap while holding the lock.
     * @param size The size of the memory block to allocate, in bytes.
     * @return Pointer to the allocated memory block, or `nullptr` if allocation fails.
     * 
     * Performs the allocation assuming that the heap lock has been acquired.
     * This method is intended for internal use only.
     */
    void* _allocate_locked(size_t size);

    /**
     * @brief Frees a previously allocated memory block while holding the lock.
     * @param ptr Pointer to the memory block to free.
     * 
     * Marks the corresponding segment as free and attempts to merge adjacent free segments.
     * This method is intended for internal use only and operates in a synchronized context.
     */
    void _free_locked(void* ptr);

    /**
     * @brief Finds a free segment that can satisfy a minimum size requirement.
     * @param min_size The minimum size of the segment, in bytes.
     * @return Pointer to a suitable free segment, or `nullptr` if none are available.
     * 
     * Searches the heap for a free segment with sufficient size.
     * This method is used internally during allocation operations.
     */
    heap_segment_header* _find_free_segment(size_t min_size);

    //
    // |-----|--------------|     |-----|------------|  |-----|------------|
    // | hdr |              | --> | hdr |            |  | hdr |            |
    // |-----|--------------|     |-----|------------|  |-----|------------|
    //
    // <-------- x --------->     <------ size ------>  <---- x - size ---->
    //
    bool _split_segment(heap_segment_header* segment, size_t size);

    /**
     * @brief Merges a segment with the previous free segment.
     * @param segment Pointer to the segment to merge.
     * @return True if the merge was successful, false otherwise.
     * 
     * Combines the specified segment with its previous neighbor if the previous segment is free.
     * This operation reduces fragmentation in the heap.
     */
    bool _merge_segment_with_previous(heap_segment_header* segment);

    /**
     * @brief Merges a segment with the next free segment.
     * @param segment Pointer to the segment to merge.
     * @return True if the merge was successful, false otherwise.
     * 
     * Combines the specified segment with its next neighbor if the next segment is free.
     * This operation reduces fragmentation in the heap.
     */
    bool _merge_segment_with_next(heap_segment_header* segment);
};
} // namespace allocators

#endif // HEAP_ALLOCATOR_H
