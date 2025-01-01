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
    mutex_guard guard(m_heap_lock);

    m_heap_size = size;
    m_first_segment = reinterpret_cast<heap_segment_header*>(base);

    if (!m_first_segment) {
        size_t pages = size / PAGE_SIZE;
        size_t large_pages = size / LARGE_PAGE_SIZE;

        /*
         * Optimization: instead of searching through the bitmap for a free
         * contiguous region, since this code is run at kernel init time,
         * we know that all allocated virtual pages will be contiguous.
         * Therefore, we can allocate and map only 1 page and then manually
         * lock all consecutive pages.
         */
        void* vbase = vmm::alloc_virtual_pages(1, PTE_DEFAULT_UNPRIV_KERNEL_FLAGS);
        page_bitmap_allocator::get_virtual_allocator().lock_pages(vbase, pages);

        m_first_segment = reinterpret_cast<heap_segment_header*>(vbase);

        // Map the rest of the heap using large pages
        for (size_t i = 0; i < large_pages; ++i) {
            void* pbase = page_bitmap_allocator::get_physical_allocator().alloc_large_page();
            if (!pbase) {
                serial::printf("Failed to allocate large page %llu\n", i);
                break;
            }

            uintptr_t paddr = reinterpret_cast<uintptr_t>(pbase);
            uintptr_t vaddr = reinterpret_cast<uintptr_t>(vbase) + (i * LARGE_PAGE_SIZE);

            paging::map_large_page(vaddr, paddr, PTE_DEFAULT_UNPRIV_KERNEL_FLAGS, paging::get_pml4());
        }
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
    mutex_guard guard(m_heap_lock);
    return _allocate_locked(size);
}

void heap_allocator::free(void* ptr) {
    mutex_guard guard(m_heap_lock);
    _free_locked(ptr);
}

void* heap_allocator::reallocate(void* ptr, size_t new_size) {
    mutex_guard guard(m_heap_lock);

    if (ptr == nullptr) {
        return _allocate_locked(new_size);
    }

    heap_segment_header* segment = reinterpret_cast<heap_segment_header*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(heap_segment_header)
    );

    if (memcmp(segment->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, 7) != 0) {
        serial::printf("Invalid pointer provided to realloc()!\n");
        return nullptr;
    }

    if (segment->size >= new_size + sizeof(heap_segment_header)) {
        _split_segment(segment, new_size + sizeof(heap_segment_header));
        return ptr;
    } else {
        void* new_ptr = _allocate_locked(new_size);
        if (!new_ptr) {
            return nullptr;
        }

        memcpy(new_ptr, ptr, segment->size - sizeof(heap_segment_header));

        _free_locked(ptr);

        return new_ptr;
    }

    return nullptr;
}

void* heap_allocator::_allocate_locked(size_t size) {
    // Check for an invalid allocation size
    if (size == 0) {
        return nullptr;
    }

    size_t new_segment_size = size + sizeof(heap_segment_header);

    heap_segment_header* segment = _find_free_segment(new_segment_size + sizeof(heap_segment_header));

    if (!segment) {
        return nullptr;
    }

    _split_segment(segment, new_segment_size);

    segment->flags.free = false;

    uint8_t* usable_region_start = reinterpret_cast<uint8_t*>(segment) + sizeof(heap_segment_header);
    return static_cast<void*>(usable_region_start);
}

void heap_allocator::_free_locked(void* ptr) {
    heap_segment_header* segment = reinterpret_cast<heap_segment_header*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(heap_segment_header)
    );

    if (memcmp(segment->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, 7) != 0) {
        serial::printf("Invalid pointer provided to free()!\n");
        return;
    }

    void* userptr = (void*)((uint64_t)segment + sizeof(heap_segment_header));
    zeromem(userptr, segment->size - sizeof(heap_segment_header));
    segment->flags.free = true;

    if (segment->prev && segment->prev->flags.free) {
        _merge_segment_with_previous(segment);
    }

    if (segment->next && segment->next->flags.free) {
        _merge_segment_with_next(segment);
    }
}

heap_segment_header* heap_allocator::_find_free_segment(size_t min_size) {
    heap_segment_header* seg = m_first_segment;

    while (seg) {
        if (seg->flags.free && (seg->size >= min_size)) {
            return seg;
        }

        seg = seg->next;
    }

    return nullptr;
}

bool heap_allocator::_split_segment(heap_segment_header* segment, size_t size) {
    if (static_cast<int64_t>(segment->size - (size + sizeof(heap_segment_header))) < MIN_HEAP_SEGMENT_CAPACITY * 2) {
        return false;
    }

    heap_segment_header* new_segment = reinterpret_cast<heap_segment_header*>(
        reinterpret_cast<uint8_t*>(segment) + size
    );

    // Clear out the segment's memory
    zeromem(new_segment, sizeof(heap_segment_header));

    // Initialize the segment
    WRITE_SEGMENT_MAGIC_FIELD(new_segment)
    new_segment->flags.free = segment->flags.free;
    new_segment->size = segment->size - size;
    new_segment->next = segment->next;
    new_segment->prev = segment;

    // Fix the next node's prev link
    if (segment->next) {
        segment->next->prev = new_segment;
    }

    segment->size = size;
    segment->next = new_segment;

    return true;
}

bool heap_allocator::_merge_segment_with_previous(heap_segment_header* segment) {
    heap_segment_header* previous_segment = segment->prev;

    if (previous_segment == nullptr) {
        return false;
    }

    previous_segment->size += segment->size;
    previous_segment->next = segment->next;

    if (previous_segment->next) {
        previous_segment->next->prev = previous_segment;
    }

    // Discard the links from the original segment
    segment->next = nullptr;
    segment->prev = nullptr;

    return true;
}

bool heap_allocator::_merge_segment_with_next(heap_segment_header* segment) {
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
    mutex_guard guard(m_heap_lock);

    heap_segment_header* seg = m_first_segment;
    int64_t seg_id = 1;

    serial::printf("---------------------------------------------\n");
    while (seg) {
        serial::printf("Segment %llu:\n", seg_id);
        serial::printf("    base         : %llx\n", (uint64_t)seg);
        serial::printf("    userptr      : %llx\n", (uint64_t)seg + sizeof(heap_segment_header));
        serial::printf("    total size   : %llx\n", seg->size);
        serial::printf("    usable size  : %llx\n", GET_USABLE_BLOCK_MEMORY_SIZE(seg));
        serial::printf("    status       : %s\n", seg->flags.free ? "free" : "used");
        serial::printf("    next         : %llx\n", (uint64_t)seg->next);
        serial::printf("    prev         : %llx\n\n", (uint64_t)seg->prev);
        
        seg_id++;
        seg = seg->next;
    }
    serial::printf("---------------------------------------------\n");
}

void heap_allocator::debug_heap_segment(void* ptr, int64_t seg_id) {
    heap_segment_header* seg = (heap_segment_header*)ptr;
    
    if (seg_id != -1)
        serial::printf("Segment %llu:\n", seg_id);
    else
        serial::printf("Segment\n");

    serial::printf("    base         : %llx\n", (uint64_t)seg);
    serial::printf("    userptr      : %llx\n", (uint64_t)seg + sizeof(heap_segment_header));
    serial::printf("    total size   : %llx\n", seg->size);
    serial::printf("    usable size  : %llx\n", GET_USABLE_BLOCK_MEMORY_SIZE(seg));
    serial::printf("    status       : %s\n", seg->flags.free ? "free" : "used");
    serial::printf("    next         : %llx\n", (uint64_t)seg->next);
    serial::printf("    prev         : %llx\n\n", (uint64_t)seg->prev);
}

void heap_allocator::debug_user_heap_pointer(void* ptr, int64_t id) {
    void* seg = (void*)((uint64_t)ptr - sizeof(heap_segment_header));
    debug_heap_segment(seg, id);
}

bool heap_allocator::detect_heap_corruption(bool dbg_log) {
    mutex_guard guard(m_heap_lock);

    heap_segment_header* seg = m_first_segment;
    int64_t seg_id = 1;

    while (seg) {
        bool corrupted = false;
        if (memcmp(seg->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, sizeof(seg->magic)) != 0) {
            corrupted = true;
            serial::printf("[!] Magic number is corrupted\n");
        } else if (seg->flags.reserved != 0) {
            corrupted = true;
            serial::printf("[!] Reserved flags were not 0\n");
        } else if (
            seg->next != nullptr &&
            reinterpret_cast<uint8_t*>(seg) + seg->size != reinterpret_cast<uint8_t*>(seg->next)
        ) {
            corrupted = true;
            serial::printf("[!] Corrupted size + next link\n");
        } else if (seg->prev && seg->prev->next != seg) {
            corrupted = true;
            serial::printf("[!] Corrupted seg->prev->next link\n");
        } else if (seg->next && seg->next->prev != seg) {
            corrupted = true;
            serial::printf("[!] Corrupted seg->next->prev link\n");
        }

        if (corrupted) {
            if (dbg_log) {
                serial::printf("---- Detected Heap Corruption (segment %lli) ----\n", seg_id);
                debug_heap_segment(seg, seg_id);
            }

            return true;
        }
        
        seg_id++;
        seg = seg->next;
    }

    if (dbg_log) {
        serial::printf("---- No Heap Corruption Detected (checked %lli segments) ----\n", seg_id - 1);
    }

    return false;
}
} // namespace allocators
