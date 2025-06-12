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
void page_bitmap_allocator::set_base_page_offset(uint64_t offset) {
    m_base_page_offset = offset;
}

__PRIVILEGED_CODE
void page_bitmap_allocator::mark_bitmap_address_as_physical() {
    m_bitmap.mark_buffer_address_as_physical();
}

__PRIVILEGED_CODE
void page_bitmap_allocator::lock_page(void* addr) {
    // Align the address to the page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);

    // Ensure the address is within the managed range
    if (addr_val < m_base_page_offset) {
        serial::printf("[*] failed to lock page: address below base offset: 0x%016llx\n", addr_val);
        return;
    }

    // Convert to relative address
    uintptr_t relative_addr_val = addr_val - m_base_page_offset;
    void* relative_addr = reinterpret_cast<void*>(relative_addr_val);

    // Mark the page as used (locked)
    bool success = m_bitmap.mark_page_used(relative_addr);
    if (!success) {
        serial::printf("[*] failed to lock page: 0x%016llx\n", aligned_addr);
    }
}

__PRIVILEGED_CODE
void page_bitmap_allocator::lock_pages(void* addr, size_t count) {
    // Align the address to the page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);

    // Ensure the address is within the managed range
    if (addr_val < m_base_page_offset) {
        serial::printf("[*] failed to lock %zu pages: address below base offset: 0x%016llx\n", count, addr_val);
        return;
    }

    // Convert to relative address
    uintptr_t relative_addr_val = addr_val - m_base_page_offset;
    void* relative_addr = reinterpret_cast<void*>(relative_addr_val);

    // Mark the pages as used (locked)
    bool success = m_bitmap.mark_pages_used(relative_addr, count);
    if (!success) {
        serial::printf("[*] failed to lock %zu pages at: 0x%016llx\n", count, aligned_addr);
    }
}

__PRIVILEGED_CODE
void page_bitmap_allocator::free_page(void* addr) {
    // Align the address to the page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);

    // Ensure the address is within the managed range
    if (addr_val < m_base_page_offset) {
        serial::printf("[*] failed to free page: address below base offset: 0x%016llx\n", addr_val);
        return;
    }

    // Convert to relative address
    uintptr_t relative_addr_val = addr_val - m_base_page_offset;
    void* relative_addr = reinterpret_cast<void*>(relative_addr_val);

    // Mark the page as free
    bool success = m_bitmap.mark_page_free(relative_addr);
    if (!success) {
        serial::printf("[*] failed to free page: 0x%016llx\n", aligned_addr);
    }
}

__PRIVILEGED_CODE
void page_bitmap_allocator::free_pages(void* addr, size_t count) {
    // Align the address to the page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);

    // Ensure the address is within the managed range
    if (addr_val < m_base_page_offset) {
        serial::printf("[*] failed to free %zu pages: address below base offset: 0x%016llx\n", count, addr_val);
        return;
    }

    // Convert to relative address
    uintptr_t relative_addr_val = addr_val - m_base_page_offset;
    void* relative_addr = reinterpret_cast<void*>(relative_addr_val);

    // Mark the pages as free
    bool success = m_bitmap.mark_pages_free(relative_addr, count);
    if (!success) {
        serial::printf("[*] failed to free %zu pages at: 0x%016llx\n", count, aligned_addr);
    }
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_page() {
    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t total_pages = bitmap.get_size(); // Total number of pages

    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = total_pages;

    // Search from next_free_index to the end
    while (index < end_index) {
        uintptr_t relative_addr_val = index * PAGE_SIZE;
        void* relative_addr = reinterpret_cast<void*>(relative_addr_val);

        if (bitmap.is_page_free(relative_addr)) {
            if (bitmap.mark_page_used(relative_addr)) {
                uintptr_t absolute_addr_val = m_base_page_offset + relative_addr_val;
                void* absolute_addr = reinterpret_cast<void*>(absolute_addr_val);
                return absolute_addr;
            } else {
                serial::printf("[*] alloc_page: failed to mark page as used at index %llu\n", index);
            }
        }
        index++;
    }

    serial::printf("[*] alloc_page: no free pages found\n");
    return nullptr;
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_pages(size_t count) {
    if (count == 0) {
        serial::printf("[*] alloc_pages: invalid count (0)\n");
        return nullptr;
    }

    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t total_pages = bitmap.get_size(); // Total number of pages

    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = total_pages;

    // Search from next_free_index to the end
    while (index + count <= end_index) {
        // Check if we have enough consecutive free pages
        bool block_free = true;
        for (size_t i = 0; i < count; ++i) {
            uint64_t current_index = index + i;
            if (current_index >= end_index) {
                block_free = false;
                break;
            }

            uintptr_t relative_addr_val = current_index * PAGE_SIZE;
            void* relative_addr = reinterpret_cast<void*>(relative_addr_val);

            if (!bitmap.is_page_free(relative_addr)) {
                block_free = false;
                break;
            }
        }

        if (block_free) {
            // Try to mark all pages as used
            uintptr_t start_relative_addr_val = index * PAGE_SIZE;
            void* start_relative_addr = reinterpret_cast<void*>(start_relative_addr_val);

                if (bitmap.mark_pages_used(start_relative_addr, count)) {
                uintptr_t absolute_start_addr_val = m_base_page_offset + start_relative_addr_val;
                    void* absolute_start_addr = reinterpret_cast<void*>(absolute_start_addr_val);
                    return absolute_start_addr;
            } else {
                serial::printf("[*] alloc_pages: failed to mark pages as used starting at index %llu\n", index);
            }
        }

        index++;
    }

    serial::printf("[*] alloc_pages: no suitable contiguous block found\n");
    return nullptr;
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_pages_aligned(size_t count, uint64_t alignment) {
    if (count == 0 || (alignment & (alignment - 1)) != 0) {
        // Alignment is not a power of two or count is zero
        return nullptr;
    }

    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t total_pages = bitmap.get_size(); // Total number of pages

    // Calculate the alignment in terms of pages
    if (alignment < PAGE_SIZE) {
        alignment = PAGE_SIZE;
    }
    uint64_t alignment_pages = alignment / PAGE_SIZE;

    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = total_pages;

    // Search from next_free_index to the end
    for (; index < end_index; ++index) {
        // Check alignment
        if ((index % alignment_pages) != 0) {
            continue;
        }

        // Check if there are enough pages remaining
        if (index + count > total_pages) {
            break;
        }

        bool block_free = true;
        for (uint64_t offset = 0; offset < count; ++offset) {
            void* relative_addr = reinterpret_cast<void*>((index + offset) * PAGE_SIZE);
            if (!bitmap.is_page_free(relative_addr)) {
                block_free = false;
                break;
            }
        }

        if (block_free) {
            void* start_relative_addr = reinterpret_cast<void*>(index * PAGE_SIZE);
            if (bitmap.mark_pages_used(start_relative_addr, count)) {
                uintptr_t absolute_start_addr_val = m_base_page_offset + (index * PAGE_SIZE);
                void* absolute_start_addr = reinterpret_cast<void*>(absolute_start_addr_val);
                return absolute_start_addr;
            }
        }
    }

    // No suitable aligned contiguous block found
    return nullptr;
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_large_page() {
    const uint64_t large_page_alignment = LARGE_PAGE_SIZE / PAGE_SIZE; // Alignment in terms of 4KB pages

    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t total_pages = bitmap.get_size(); // Total number of 4KB pages
    uint64_t start_index = bitmap.get_next_free_index();
    uint64_t index = start_index;
    uint64_t end_index = total_pages;

    // Search from next_free_index to the end in steps of large_page_alignment
    while (index + large_page_alignment <= end_index) {
        // Check if the block is free and aligned for a large page
        bool block_free = true;
        for (uint64_t offset = 0; offset < large_page_alignment; ++offset) {
            void* relative_addr = reinterpret_cast<void*>((index + offset) * PAGE_SIZE);
            if (!bitmap.is_page_free(relative_addr)) {
                block_free = false;
                break;
            }
        }

        if (block_free) {
            void* start_relative_addr = reinterpret_cast<void*>(index * PAGE_SIZE);
            if (bitmap.mark_pages_used(start_relative_addr, large_page_alignment)) {
                uintptr_t absolute_start_addr_val = m_base_page_offset + (index * PAGE_SIZE);
                void* absolute_start_addr = reinterpret_cast<void*>(absolute_start_addr_val);
                return absolute_start_addr;
            }
        }

        // Move to next potential alignment boundary
        index += large_page_alignment;
        
        // If we've searched the entire bitmap, break to avoid infinite loop
        if (index >= end_index) {
            break;
        }
    }

    // No free 2MB page found
    return nullptr;
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_large_pages(size_t count) {
    if (count == 0) {
        serial::printf("[*] alloc_large_pages: invalid count (0)\n");
        return nullptr;
    }

    const uint64_t large_page_size = 2 * 1024 * 1024; // 2MB
    const uint64_t large_page_alignment = large_page_size / PAGE_SIZE; // Alignment in terms of 4KB pages

    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t total_pages = bitmap.get_size(); // Total number of 4KB pages
    uint64_t start_index = bitmap.get_next_free_index();
    uint64_t index = start_index;
    uint64_t end_index = total_pages;

    serial::printf("[*] alloc_large_pages: searching for %zu large pages (total pages: %llu, start index: %llu)\n", 
                  count, total_pages, start_index);

    // Search from next_free_index to the end in steps of large_page_alignment
    while (index + large_page_alignment <= end_index) {
        // Check if the block is free and aligned for a large page
        bool block_free = true;
        for (uint64_t offset = 0; offset < large_page_alignment; ++offset) {
            void* relative_addr = reinterpret_cast<void*>((index + offset) * PAGE_SIZE);
            if (!bitmap.is_page_free(relative_addr)) {
                block_free = false;
                break;
            }
        }

        if (block_free) {
            serial::printf("[*] alloc_large_pages: found potential block at index %llu\n", index);
            
            // Check if we have enough consecutive large pages
            bool has_enough_pages = true;
            for (size_t i = 0; i < count; ++i) {
                uint64_t current_index = index + (i * large_page_alignment);
                if (current_index + large_page_alignment > end_index) {
                    serial::printf("[*] alloc_large_pages: not enough space at index %llu\n", current_index);
                    has_enough_pages = false;
                    break;
                }
                
                for (uint64_t offset = 0; offset < large_page_alignment; ++offset) {
                    void* relative_addr = reinterpret_cast<void*>((current_index + offset) * PAGE_SIZE);
                    if (!bitmap.is_page_free(relative_addr)) {
                        serial::printf("[*] alloc_large_pages: page not free at index %llu + offset %llu\n", 
                                     current_index, offset);
                        has_enough_pages = false;
                        break;
                    }
                }
                if (!has_enough_pages) break;
            }

            if (has_enough_pages) {
                void* start_relative_addr = reinterpret_cast<void*>(index * PAGE_SIZE);
                if (bitmap.mark_pages_used(start_relative_addr, count * large_page_alignment)) {
                    uintptr_t absolute_start_addr_val = m_base_page_offset + (index * PAGE_SIZE);
                    void* absolute_start_addr = reinterpret_cast<void*>(absolute_start_addr_val);
                    serial::printf("[*] alloc_large_pages: successfully allocated %zu large pages at 0x%016llx\n", 
                                 count, absolute_start_addr_val);
                    return absolute_start_addr;
                } else {
                    serial::printf("[*] alloc_large_pages: failed to mark pages as used at index %llu\n", index);
            }
            }
        }

        // Move to next potential alignment boundary
        index += large_page_alignment;
        
        // If we've searched the entire bitmap, break to avoid infinite loop
        if (index >= end_index) {
            serial::printf("[*] alloc_large_pages: reached end of bitmap without finding suitable block\n");
            break;
        }
    }

    serial::printf("[*] alloc_large_pages: no suitable contiguous large pages found\n");
    return nullptr;
}

__PRIVILEGED_CODE
void page_bitmap_allocator::free_large_page(void* addr) {
    // Align the address to the large page boundary
    uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
    addr_val &= ~(LARGE_PAGE_SIZE - 1);
    void* aligned_addr = reinterpret_cast<void*>(addr_val);

    // Ensure the address is within the managed range
    if (addr_val < m_base_page_offset) {
        serial::printf("[*] failed to free large page: address below base offset: 0x%016llx\n", addr_val);
        return;
    }

    // Convert to relative address
    uintptr_t relative_addr_val = addr_val - m_base_page_offset;
    void* relative_addr = reinterpret_cast<void*>(relative_addr_val);

    // Mark the large page as free (512 4KB pages)
    bool success = m_bitmap.mark_pages_free(relative_addr, LARGE_PAGE_SIZE / PAGE_SIZE);
    if (!success) {
        serial::printf("[*] failed to free large page: 0x%016llx\n", aligned_addr);
    }
}
} // namespace allocators
