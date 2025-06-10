#include <stlibc/memory/heap.h>
#include <stlibc/memory/memory.h>
#include <stlibc/memory/mman.h>
#include <serial/serial.h>

namespace stlibc {

// Global heap allocator instance
static heap_allocator g_heap_allocator;

heap_allocator& heap_allocator::get() {
    static bool initialized = false;
    if (!initialized) {
        g_heap_allocator.init();
        initialized = true;
    }
    return g_heap_allocator;
}

void heap_allocator::init() {
    // Allocate initial heap space using mmap
    m_heap_start = mmap(nullptr, HEAP_INIT_SIZE, PROT_READ | PROT_WRITE, 0);
    if (!m_heap_start) {
        // If mmap fails, we can't continue
        return;
    }

    m_heap_size = HEAP_INIT_SIZE;
    m_heap_end = static_cast<void*>(static_cast<char*>(m_heap_start) + m_heap_size);
    m_first_segment = static_cast<heap_segment_header*>(m_heap_start);

    // Initialize the first segment
    memcpy(m_first_segment->magic, HEAP_SEGMENT_HDR_SIGNATURE, sizeof(m_first_segment->magic));
    m_first_segment->flags.free = true;
    m_first_segment->flags.reserved = 0;
    m_first_segment->size = m_heap_size;
    m_first_segment->next = nullptr;
    m_first_segment->prev = nullptr;
}

void* heap_allocator::allocate(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    // Add header size to the requested size
    size_t total_size = size + sizeof(heap_segment_header);
    
    // Align size to 16 bytes for better performance
    total_size = (total_size + 15) & ~15;

    return _allocate_locked(total_size);
}

void heap_allocator::free(void* ptr) {
    if (!ptr) {
        return;
    }
    _free_locked(ptr);
}

void* heap_allocator::reallocate(void* ptr, size_t new_size) {
    if (!ptr) {
        return allocate(new_size);
    }

    if (new_size == 0) {
        free(ptr);
        return nullptr;
    }

    heap_segment_header* segment = reinterpret_cast<heap_segment_header*>(
        static_cast<char*>(ptr) - sizeof(heap_segment_header)
    );

    // Verify magic number
    if (memcmp(segment->magic, HEAP_SEGMENT_HDR_SIGNATURE, sizeof(segment->magic)) != 0) {
        return nullptr;
    }

    size_t current_size = segment->size - sizeof(heap_segment_header);
    if (current_size >= new_size) {
        // If we have enough space, try to split the block
        size_t total_new_size = new_size + sizeof(heap_segment_header);
        if (_split_segment(segment, total_new_size)) {
            return ptr;
        }
        return ptr;  // Keep the original size if split fails
    }

    // Need to allocate new block and copy data
    void* new_ptr = allocate(new_size);
    if (!new_ptr) {
        return nullptr;
    }

    memcpy(new_ptr, ptr, current_size);
    free(ptr);
    return new_ptr;
}

void* heap_allocator::_allocate_locked(size_t size) {
    heap_segment_header* segment = _find_free_segment(size);
    if (!segment) {
        // Try to expand the heap
        if (!_expand_heap(size)) {
            return nullptr;
        }
        segment = _find_free_segment(size);
        if (!segment) {
            return nullptr;
        }
    }

    // Split the segment if it's too large
    if (segment->size >= size + sizeof(heap_segment_header) + 32) {  // 32 bytes minimum for a new segment
        _split_segment(segment, size);
    }

    segment->flags.free = false;
    return static_cast<void*>(reinterpret_cast<char*>(segment) + sizeof(heap_segment_header));
}

void heap_allocator::_free_locked(void* ptr) {
    heap_segment_header* segment = reinterpret_cast<heap_segment_header*>(
        static_cast<char*>(ptr) - sizeof(heap_segment_header)
    );

    // Verify magic number
    if (memcmp(segment->magic, HEAP_SEGMENT_HDR_SIGNATURE, sizeof(segment->magic)) != 0) {
        return;
    }

    // Mark the segment as free
    segment->flags.free = true;

    // Try to merge with adjacent free segments
    if (segment->prev && segment->prev->flags.free) {
        _merge_segment_with_previous(segment);
    }
    if (segment->next && segment->next->flags.free) {
        _merge_segment_with_next(segment);
    }
}

heap_segment_header* heap_allocator::_find_free_segment(size_t min_size) {
    heap_segment_header* current = m_first_segment;
    while (current) {
        if (current->flags.free && current->size >= min_size) {
            return current;
        }
        current = current->next;
    }
    return nullptr;
}

bool heap_allocator::_split_segment(heap_segment_header* segment, size_t size) {
    if (segment->size < size + sizeof(heap_segment_header) + 32) {  // 32 bytes minimum for a new segment
        return false;
    }

    heap_segment_header* new_segment = reinterpret_cast<heap_segment_header*>(
        reinterpret_cast<char*>(segment) + size
    );

    // Initialize new segment
    memcpy(new_segment->magic, HEAP_SEGMENT_HDR_SIGNATURE, sizeof(new_segment->magic));
    new_segment->flags.free = true;
    new_segment->flags.reserved = 0;
    new_segment->size = segment->size - size;
    new_segment->next = segment->next;
    new_segment->prev = segment;

    // Update segment
    segment->size = size;
    segment->next = new_segment;

    // Update next segment's prev pointer
    if (new_segment->next) {
        new_segment->next->prev = new_segment;
    }

    return true;
}

bool heap_allocator::_merge_segment_with_previous(heap_segment_header* segment) {
    if (!segment->prev || !segment->prev->flags.free) {
        return false;
    }

    heap_segment_header* prev = segment->prev;
    prev->size += segment->size;
    prev->next = segment->next;
    
    if (segment->next) {
        segment->next->prev = prev;
    }

    return true;
}

bool heap_allocator::_merge_segment_with_next(heap_segment_header* segment) {
    if (!segment->next || !segment->next->flags.free) {
        return false;
    }

    heap_segment_header* next = segment->next;
    segment->size += next->size;
    segment->next = next->next;
    
    if (next->next) {
        next->next->prev = segment;
    }

    return true;
}

bool heap_allocator::_expand_heap(size_t size) {
    // Calculate new size (double the current size or size + current size, whichever is larger)
    size_t new_size = m_heap_size * 2;
    if (new_size < m_heap_size + size) {
        new_size = m_heap_size + size;
    }

    // Align to page size
    new_size = (new_size + 4095) & ~4095;

    // Map new memory
    void* new_memory = mmap(
        m_heap_end,
        new_size - m_heap_size, 
        PROT_READ | PROT_WRITE, 
        0
    );
    
    if (new_memory != m_heap_end) {
        return false;
    }

    // Create new segment for the expanded area
    heap_segment_header* new_segment = static_cast<heap_segment_header*>(m_heap_end);
    memcpy(new_segment->magic, HEAP_SEGMENT_HDR_SIGNATURE, sizeof(new_segment->magic));
    new_segment->flags.free = true;
    new_segment->flags.reserved = 0;
    new_segment->size = new_size - m_heap_size;
    new_segment->next = nullptr;
    new_segment->prev = m_first_segment;

    // Update heap state
    m_heap_size = new_size;
    m_heap_end = static_cast<void*>(static_cast<char*>(m_heap_start) + m_heap_size);

    // Merge with last segment if it's free
    if (m_first_segment->flags.free) {
        _merge_segment_with_next(m_first_segment);
    }

    return true;
}

void heap_allocator::debug_heap() {
    heap_segment_header* current = m_first_segment;
    int64_t seg_id = 1;

    while (current) {
        debug_heap_segment(current, seg_id++);
        current = current->next;
    }
}

void heap_allocator::debug_heap_segment(void* ptr, int64_t seg_id) {
    heap_segment_header* segment = static_cast<heap_segment_header*>(ptr);
    __unused segment;
    
    if (seg_id != -1) {
        // TODO: Replace with proper logging
        // printf("Segment %lld:\n", seg_id);
    } else {
        // printf("Segment:\n");
    }
    
    // printf("    base         : %p\n", segment);
    // printf("    userptr      : %p\n", static_cast<char*>(ptr) + sizeof(heap_segment_header));
    // printf("    total size   : %zu\n", segment->size);
    // printf("    usable size  : %zu\n", segment->size - sizeof(heap_segment_header));
    // printf("    status       : %s\n", segment->flags.free ? "free" : "used");
    // printf("    next         : %p\n", segment->next);
    // printf("    prev         : %p\n\n", segment->prev);
}

void heap_allocator::debug_user_heap_pointer(void* ptr, int64_t id) {
    void* seg = static_cast<char*>(ptr) - sizeof(heap_segment_header);
    debug_heap_segment(seg, id);
}

bool heap_allocator::detect_heap_corruption(bool dbg_log) {
    heap_segment_header* current = m_first_segment;
    int64_t seg_id = 1;

    while (current) {
        bool corrupted = false;
        
        if (memcmp(current->magic, HEAP_SEGMENT_HDR_SIGNATURE, sizeof(current->magic)) != 0) {
            corrupted = true;
            // printf("[!] Magic number is corrupted\n");
        } else if (current->flags.reserved != 0) {
            corrupted = true;
            // printf("[!] Reserved flags were not 0\n");
        } else if (
            current->next != nullptr &&
            reinterpret_cast<char*>(current) + current->size != reinterpret_cast<char*>(current->next)
        ) {
            corrupted = true;
            // printf("[!] Corrupted size + next link\n");
        } else if (current->prev && current->prev->next != current) {
            corrupted = true;
            // printf("[!] Corrupted current->prev->next link\n");
        } else if (current->next && current->next->prev != current) {
            corrupted = true;
            // printf("[!] Corrupted current->next->prev link\n");
        }

        if (corrupted) {
            if (dbg_log) {
                // printf("---- Detected Heap Corruption (segment %lld) ----\n", seg_id);
                debug_heap_segment(current, seg_id);
            }
            return true;
        }
        
        seg_id++;
        current = current->next;
    }

    if (dbg_log) {
        // printf("---- No Heap Corruption Detected (checked %lld segments) ----\n", seg_id - 1);
    }

    return false;
}

} // namespace stlibc 