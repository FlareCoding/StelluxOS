#include <memory/allocators/page_bitmap_allocator.h>
#include <memory/memory.h>
#include <memory/page_bitmap.h>
#include <memory/paging.h>
#include <serial/serial.h>

namespace allocators {
__PRIVILEGED_CODE
page_bitmap_allocator& page_bitmap_allocator::get_physical_allocator() {
    GENERATE_STATIC_SINGLETON(page_bitmap_allocator);
}

__PRIVILEGED_CODE
page_bitmap_allocator& page_bitmap_allocator::get_virtual_allocator() {
    GENERATE_STATIC_SINGLETON(page_bitmap_allocator);
}

__PRIVILEGED_CODE
void page_bitmap_allocator::init_bitmap(uint64_t size, uint8_t* buffer, bool initial_used_value) {
    m_bitmap.init(size, buffer, initial_used_value);
}

__PRIVILEGED_CODE
void page_bitmap_allocator::lock_page(void* addr) {
    // Align the address to the page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);
    
    // Mark the page as used (locked)
    bool success = m_bitmap.mark_page_used(aligned_addr);
    if (!success) {
        serial::com1_printf("[*] failed to lock page: 0x%016llx\n", aligned_addr);
    }
}

__PRIVILEGED_CODE
void page_bitmap_allocator::lock_pages(void* addr, size_t count) {
    // Align the address to the page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);
    
    // Mark the pages as used (locked)
    bool success = m_bitmap.mark_pages_used(aligned_addr, count);
    if (!success) {
        serial::com1_printf("[*] failed to lock %u pages at: 0x%016llx\n", count, aligned_addr);
    }
}

__PRIVILEGED_CODE
void page_bitmap_allocator::free_page(void* addr) {
    // Align the address to the page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);
    
    // Mark the page as free
    bool success = m_bitmap.mark_page_free(aligned_addr);
    if (!success) {
        serial::com1_printf("[*] failed to free page: 0x%016llx\n", aligned_addr);
    }
}

__PRIVILEGED_CODE
void page_bitmap_allocator::free_pages(void* addr, size_t count) {
    // Align the address to the page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);
    
    // Mark the pages as free
    bool success = m_bitmap.mark_pages_free(aligned_addr, count);
    if (!success) {
        serial::com1_printf("[*] failed to free %u pages at: 0x%016llx\n", count, aligned_addr);
    }
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_page() {
    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t size = bitmap.get_size() * 8; // Total number of pages

    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = size;

    // Search from _next_free_index to the end
    for (; index < end_index; ++index) {
        uintptr_t paddr = index * PAGE_SIZE;
        void* addr = reinterpret_cast<void*>(paddr);

        if (bitmap.is_page_free(addr)) {
            if (bitmap.mark_page_used(addr)) {
                return addr;
            }
        }
    }

    // No free page found
    return nullptr;
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_pages(size_t count) {
    if (count == 0) {
        return nullptr;
    }

    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t size = bitmap.get_size() * 8; // Total number of pages

    size_t consecutive = 0;
    uint64_t start_index = 0;
    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = size;

    // Search from _next_free_index to the end
    for (; index < end_index; ++index) {
        void* addr = reinterpret_cast<void*>(index * PAGE_SIZE);

        if (bitmap.is_page_free(addr)) {
            if (consecutive == 0) {
                start_index = index;
            }
            consecutive++;

            if (consecutive == count) {
                void* start_addr = reinterpret_cast<void*>(start_index * PAGE_SIZE);
                if (bitmap.mark_pages_used(start_addr, count)) {
                    return start_addr;
                }
                consecutive = 0;
            }
        } else {
            consecutive = 0;
        }
    }

    // No suitable contiguous block found
    return nullptr;
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_pages_aligned(size_t count, uint64_t alignment) {
    if (count == 0 || (alignment & (alignment - 1)) != 0) {
        // Alignment is not a power of two or count is zero
        return nullptr;
    }

    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t size = bitmap.get_size() * 8; // Total number of pages

    // Calculate the alignment in terms of pages
    if (alignment < PAGE_SIZE) {
        alignment = PAGE_SIZE;
    }
    uint64_t alignment_pages = alignment / PAGE_SIZE;

    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = size;

    // Search from _next_free_index to the end
    for (; index < end_index; ++index) {
        // Check alignment
        if ((index % alignment_pages) != 0) {
            continue;
        }

        // Check if there are enough pages remaining
        if (index + count > size) {
            break;
        }

        bool block_free = true;
        for (uint64_t offset = 0; offset < count; ++offset) {
            void* current_addr = reinterpret_cast<void*>((index + offset) * PAGE_SIZE);
            if (!bitmap.is_page_free(current_addr)) {
                block_free = false;
                break;
            }
        }

        if (block_free) {
            void* start_addr = reinterpret_cast<void*>(index * PAGE_SIZE);
            if (bitmap.mark_pages_used(start_addr, count)) {
                return start_addr;
            }
        }
    }

    // No suitable aligned contiguous block found
    return nullptr;
}
} // namespace allocators
