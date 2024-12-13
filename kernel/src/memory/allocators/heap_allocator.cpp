#include <memory/allocators/heap_allocator.h>
#include <memory/vmm.h>
#include <serial/serial.h>
#include <memory/memory.h>
#include <memory/paging.h>

#define MIN_HEAP_SEGMENT_CAPACITY 1

#define GET_USABLE_BLOCK_MEMORY_SIZE(seg) seg->size - sizeof(heap_segment_header)

#define WRITE_SEGMENT_MAGIC_FIELD(seg) \
    memcpy(seg->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, sizeof(seg->magic));

namespace allocators {
heap_allocator g_kernel_heap_allocator;

heap_allocator& heap_allocator::get() {
    return g_kernel_heap_allocator;
}

void heap_allocator::init(uint64_t base, size_t size) {
    spinlock_guard guard(m_heap_lock);

    m_heap_size = size;
    m_first_segment = reinterpret_cast<heap_segment_header*>(base);

    if (!m_first_segment) {
        size_t pages = size / PAGE_SIZE;

        /*
         * Optimization: instead of searching through the bitmap for a free
         * contiguous region, since this code is run at kernel init time,
         * we know that all allocated virtual pages will be contiguous.
         * Therefore, we can allocate 1 page and then lock all consecutive pages.
         */
        void* vbase = vmm::alloc_virtual_pages(1, PTE_DEFAULT_KERNEL_FLAGS);
        page_bitmap_allocator::get_virtual_allocator().lock_pages(vbase, pages);

        m_first_segment = reinterpret_cast<heap_segment_header*>(vbase);
    }

    // Setup the root segment
    WRITE_SEGMENT_MAGIC_FIELD(m_first_segment);
    m_first_segment->flags = {
        .free = true,
        .reserved = 0
    };
    m_first_segment->size = size;
    m_first_segment->next = nullptr;
    m_first_segment->prev = nullptr;
}

void* heap_allocator::allocate(size_t size) {
    spinlock_guard guard(m_heap_lock);

    size_t new_segment_size = size + sizeof(heap_segment_header);

    heap_segment_header* segment = find_free_segment(new_segment_size + sizeof(heap_segment_header));

    if (!segment) {
        return nullptr;
    }

    split_segment(segment, new_segment_size);

    segment->flags.free = false;

    uint8_t* usable_region_start = reinterpret_cast<uint8_t*>(segment) + sizeof(heap_segment_header);
    return static_cast<void*>(usable_region_start);
}

void heap_allocator::free(void* ptr) {
    spinlock_guard guard(m_heap_lock);

    heap_segment_header* segment = reinterpret_cast<heap_segment_header*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(heap_segment_header)
    );

    if (memcmp(segment->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, 7) != 0) {
        serial::com1_printf("Invalid pointer provided to free()!\n");
        return;
    }

    segment->flags.free = true;

    if (segment->next && segment->next->flags.free) {
        merge_segment_with_next(segment);
    }

    if (segment->prev && segment->prev->flags.free) {
        merge_segment_with_previous(segment);
    }
}

void* heap_allocator::reallocate(void* ptr, size_t new_size) {
    spinlock_guard guard(m_heap_lock);

    if (ptr == nullptr) {
        return allocate(new_size);
    }

    heap_segment_header* segment = reinterpret_cast<heap_segment_header*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(heap_segment_header)
    );

    if (memcmp(segment->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, 7) != 0) {
        serial::com1_printf("Invalid pointer provided to realloc()!\n");
        return nullptr;
    }

    if (segment->size >= new_size + sizeof(heap_segment_header)) {
        split_segment(segment, new_size + sizeof(heap_segment_header));
        return ptr;
    } else {
        void* new_ptr = allocate(new_size);
        if (!new_ptr) {
            return nullptr;
        }

        memcpy(new_ptr, ptr, segment->size - sizeof(heap_segment_header));

        free(ptr);

        return new_ptr;
    }

    return nullptr;
}

heap_segment_header* heap_allocator::find_free_segment(size_t min_size) {
    heap_segment_header* seg = m_first_segment;

    while (seg) {
        if (seg->flags.free && (seg->size >= min_size)) {
            return seg;
        }

        seg = seg->next;
    }

    return nullptr;
}

bool heap_allocator::split_segment(heap_segment_header* segment, size_t size) {
    if (static_cast<int64_t>(segment->size - (size + sizeof(heap_segment_header))) < MIN_HEAP_SEGMENT_CAPACITY * 2) {
        return false;
    }

    heap_segment_header* new_segment = reinterpret_cast<heap_segment_header*>(
        reinterpret_cast<uint8_t*>(segment) + size
    );

    WRITE_SEGMENT_MAGIC_FIELD(new_segment)
    new_segment->flags.free = segment->flags.free;
    new_segment->size = segment->size - size;
    new_segment->next = segment->next;
    new_segment->prev = segment;

    segment->size = size;
    segment->next = new_segment;

    return true;
}

bool heap_allocator::merge_segment_with_previous(heap_segment_header* segment) {
    heap_segment_header* previous_segment = segment->prev;

    if (previous_segment == nullptr) {
        return false;
    }

    previous_segment->size += segment->size;
    previous_segment->next = segment->next;

    if (previous_segment->next) {
        previous_segment->next->prev = previous_segment;
    }

    return true;
}

bool heap_allocator::merge_segment_with_next(heap_segment_header* segment) {
    heap_segment_header* next_segment = segment->next;

    if (next_segment == nullptr) {
        return false;
    }

    segment->size += next_segment->size;
    segment->next = next_segment->next;

    if (segment->next) {
        segment->next->prev = segment;
    }

    return true;
}

void heap_allocator::debug_heap() {
    spinlock_guard guard(m_heap_lock);

    heap_segment_header* seg = m_first_segment;
    int64_t seg_id = 1;

    serial::com1_printf("---------------------------------------------\n");
    while (seg) {
        serial::com1_printf("Segment %llu:\n", seg_id);
        serial::com1_printf("    base         : %llx\n", (uint64_t)seg);
        serial::com1_printf("    userptr      : %llx\n", (uint64_t)seg + sizeof(heap_segment_header));
        serial::com1_printf("    total size   : %llx\n", seg->size);
        serial::com1_printf("    usable size  : %llx\n", GET_USABLE_BLOCK_MEMORY_SIZE(seg));
        serial::com1_printf("    status       : %s\n", seg->flags.free ? "free" : "used");
        serial::com1_printf("    next         : %llx\n", (uint64_t)seg->next);
        serial::com1_printf("    prev         : %llx\n\n", (uint64_t)seg->prev);
        
        seg_id++;
        seg = seg->next;
    }
    serial::com1_printf("---------------------------------------------\n");
}

void heap_allocator::debug_heap_segment(void* ptr, int64_t seg_id) {
    spinlock_guard guard(m_heap_lock);

    heap_segment_header* seg = (heap_segment_header*)ptr;
    
    if (seg_id != -1)
        serial::com1_printf("Segment %llu:\n", seg_id);
    else
        serial::com1_printf("Segment\n");

    serial::com1_printf("    base         : %llx\n", (uint64_t)seg);
    serial::com1_printf("    userptr      : %llx\n", (uint64_t)seg + sizeof(heap_segment_header));
    serial::com1_printf("    total size   : %llx\n", seg->size);
    serial::com1_printf("    usable size  : %llx\n", GET_USABLE_BLOCK_MEMORY_SIZE(seg));
    serial::com1_printf("    status       : %s\n", seg->flags.free ? "free" : "used");
    serial::com1_printf("    next         : %llx\n", (uint64_t)seg->next);
    serial::com1_printf("    prev         : %llx\n\n", (uint64_t)seg->prev);
}

void heap_allocator::debug_user_heap_pointer(void* ptr, int64_t id) {
    void* seg = (void*)((uint64_t)ptr - sizeof(heap_segment_header));
    debug_heap_segment(seg, id);
}

bool heap_allocator::detect_heap_corruption(bool dbg_log) {
    spinlock_guard guard(m_heap_lock);

    heap_segment_header* seg = m_first_segment;
    int64_t seg_id = 1;

    while (seg) {
        if (memcmp(seg->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, sizeof(seg->magic)) != 0) {
            if (dbg_log) {
                serial::com1_printf("---- Detected Heap Corruption (segment %lli) ----\n", seg_id);
                debug_heap_segment(seg, seg_id);
            }

            return true;
        }
        
        seg_id++;
        seg = seg->next;
    }

    if (dbg_log) {
        serial::com1_printf("---- No Heap Corruption Detected (checked %lli segments) ----\n", seg_id - 1);
    }

    return false;
}
} // namespace allocators
