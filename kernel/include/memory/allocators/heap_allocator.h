#ifndef HEAP_ALLOCATOR_H
#define HEAP_ALLOCATOR_H
#include <sync.h>

#define KERNEL_HEAP_INIT_SIZE               0x12C00000 // 300MB Kernel Heap

#define KERNEL_HEAP_SEGMENT_HDR_SIGNATURE   "HEAPHDR"

namespace allocators {
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

class heap_allocator {
public:
    static heap_allocator& get();

    void init(uintptr_t virt_base, size_t size);

    inline void* get_heap_base() const { return static_cast<void*>(m_first_segment); }

    void* allocate(size_t size);
    void free(void* ptr);
    void* reallocate(void* ptr, size_t new_size);

    void debug_heap();
    void debug_heap_segment(void* ptr, int64_t seg_id = -1);
    void debug_user_heap_pointer(void* ptr, int64_t id = -1);

    bool detect_heap_corruption(bool dbg_log = true);

private:
    uint64_t                m_heap_size;
    heap_segment_header*    m_first_segment;

    // Call the constructor to ensure full object
    // construction including the base class.
    mutex                   m_heap_lock = mutex();

private:
    heap_segment_header* find_free_segment(size_t min_size);

    //
    // |-----|--------------|     |-----|------------|  |-----|------------|
    // | hdr |              | --> | hdr |            |  | hdr |            |
    // |-----|--------------|     |-----|------------|  |-----|------------|
    //
    // <-------- x --------->     <------ size ------>  <---- x - size ---->
    //
    bool split_segment(heap_segment_header* segment, size_t size);

    bool merge_segment_with_previous(heap_segment_header* segment);
    bool merge_segment_with_next(heap_segment_header* segment);
};
} // namespace allocators

#endif // HEAP_ALLOCATOR_H
