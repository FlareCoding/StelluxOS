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
    for (; index < end_index; ++index) {
        uintptr_t relative_addr_val = index * PAGE_SIZE;
        void* relative_addr = reinterpret_cast<void*>(relative_addr_val);

        if (bitmap.is_page_free(relative_addr)) {
            if (bitmap.mark_page_used(relative_addr)) {
                uintptr_t absolute_addr_val = m_base_page_offset + relative_addr_val;
                void* absolute_addr = reinterpret_cast<void*>(absolute_addr_val);
                return absolute_addr;
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
    uint64_t total_pages = bitmap.get_size(); // Total number of pages

    size_t consecutive = 0;
    uint64_t start_index = 0;
    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = total_pages;

    // Search from next_free_index to the end
    for (; index < end_index; ++index) {
        void* relative_addr = reinterpret_cast<void*>(index * PAGE_SIZE);

        if (bitmap.is_page_free(relative_addr)) {
            if (consecutive == 0) {
                start_index = index;
            }
            consecutive++;

            if (consecutive == count) {
                void* start_relative_addr = reinterpret_cast<void*>(start_index * PAGE_SIZE);
                if (bitmap.mark_pages_used(start_relative_addr, count)) {
                    uintptr_t absolute_start_addr_val = m_base_page_offset + (start_index * PAGE_SIZE);
                    void* absolute_start_addr = reinterpret_cast<void*>(absolute_start_addr_val);
                    return absolute_start_addr;
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

    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = total_pages;

    // Search from next_free_index to the end in steps of large_page_alignment
    for (; index + large_page_alignment <= end_index; index += large_page_alignment) {
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
    }

    // No free 2MB page found
    return nullptr;
}

__PRIVILEGED_CODE
void* page_bitmap_allocator::alloc_large_pages(size_t count) {
    if (count == 0) {
        return nullptr;
    }

    const uint64_t large_page_size = 2 * 1024 * 1024; // 2MB
    const uint64_t large_page_alignment = large_page_size / PAGE_SIZE; // Alignment in terms of 4KB pages

    paging::page_frame_bitmap& bitmap = m_bitmap;
    uint64_t total_pages = bitmap.get_size(); // Total number of 4KB pages

    size_t consecutive = 0;
    uint64_t start_index = 0;
    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = total_pages;

    // Search from next_free_index to the end in steps of large_page_alignment
    for (; index + large_page_alignment <= end_index; index += large_page_alignment) {
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
            if (consecutive == 0) {
                start_index = index;
            }
            consecutive++;

            if (consecutive == count) {
                void* start_relative_addr = reinterpret_cast<void*>(start_index * PAGE_SIZE);
                if (bitmap.mark_pages_used(start_relative_addr, count * large_page_alignment)) {
                    uintptr_t absolute_start_addr_val = m_base_page_offset + (start_index * PAGE_SIZE);
                    void* absolute_start_addr = reinterpret_cast<void*>(absolute_start_addr_val);
                    return absolute_start_addr;
                }
                consecutive = 0;
            }
        } else {
            consecutive = 0;
        }
    }

    // No suitable contiguous large pages found
    return nullptr;
}
} // namespace allocators
