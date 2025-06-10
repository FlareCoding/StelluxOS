#ifndef STLIBC_HEAP_H
#define STLIBC_HEAP_H

#include <stlibc/stddef.h>

#define HEAP_INIT_SIZE               0x1000000  // 16MB initial heap size
#define HEAP_SEGMENT_HDR_SIGNATURE   "HEAPHDR"

namespace stlibc {

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

    void init();
    void* allocate(size_t size);
    void free(void* ptr);
    void* reallocate(void* ptr, size_t new_size);

    // Debug functions
    void debug_heap();
    void debug_heap_segment(void* ptr, int64_t seg_id = -1);
    void debug_user_heap_pointer(void* ptr, int64_t id = -1);
    bool detect_heap_corruption(bool dbg_log = true);

private:
    uint64_t                m_heap_size;
    heap_segment_header*    m_first_segment;
    void*                   m_heap_start;
    void*                   m_heap_end;

    // Internal allocation functions
    void* _allocate_locked(size_t size);
    void _free_locked(void* ptr);
    heap_segment_header* _find_free_segment(size_t min_size);
    bool _split_segment(heap_segment_header* segment, size_t size);
    bool _merge_segment_with_previous(heap_segment_header* segment);
    bool _merge_segment_with_next(heap_segment_header* segment);

    // Memory management
    bool _expand_heap(size_t size);
    void _contract_heap(size_t size);
};

} // namespace stlibc

#endif // STLIBC_HEAP_H 