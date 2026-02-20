#ifndef STELLUX_MM_HEAP_INTERNAL_H
#define STELLUX_MM_HEAP_INTERNAL_H

#include "common/types.h"
#include "mm/pmm_types.h"
#include "mm/paging_types.h"
#include "mm/kva.h"

namespace heap {

constexpr uint32_t SLAB_MAGIC = 0x534C4142; // "SLAB"
constexpr uint8_t  CLASS_COUNT = 8;
constexpr uint16_t CLASS_SIZES[CLASS_COUNT] = {16, 32, 64, 128, 256, 512, 1024, 2048};
constexpr size_t   LARGE_THRESHOLD = 2048;

enum class heap_type : uint8_t {
    privileged   = 0,
    unprivileged = 1,
};

// Embedded at the start of each slab page.
// Slab linkage (next/prev) uses VA pointers because vmm::alloc() returns
// KVA addresses. Freelist pointers in free objects are also KVA addresses.
struct slab_header {
    uint32_t     magic;
    uint32_t     _pad;
    void*        freelist; // head of embedded free list
    slab_header* next;     // next slab in partial list
    slab_header* prev;     // prev slab in partial list
};
static_assert(sizeof(slab_header) == 32);

struct slab_class {
    slab_header* partial_head; // first partial slab (VA), nullptr if none
    uint16_t     obj_size;
    uint16_t     objs_per_slab;
    uint64_t     total_allocs;
    uint64_t     total_frees;
};

struct heap_state {
    slab_class           classes[CLASS_COUNT];
    paging::page_flags_t page_flags; // PAGE_KERNEL_RW or PAGE_USER_RW
    kva::tag             heap_tag;
    heap_type            type;
};

} // namespace heap

#endif // STELLUX_MM_HEAP_INTERNAL_H
