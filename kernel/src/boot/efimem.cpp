#include <boot/efimem.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

namespace efi {
__PRIVILEGED_CODE
efi_memory_map::efi_memory_map(multiboot_tag_efi_mmap* efi_mmap_tag)
    : m_efi_mmap_tag(efi_mmap_tag), m_total_system_memory(0), m_total_conventional_memory(0) {
    m_descr_size = m_efi_mmap_tag->descr_size;

    // Calculate the number of EFI memory descriptors
    uint32_t efi_mmap_size = m_efi_mmap_tag->size - sizeof(multiboot_tag_efi_mmap);
    m_num_entries = efi_mmap_size / m_descr_size;

    // Initialize the largest conventional segment
    m_largest_conventional_segment.desc = nullptr;
    m_largest_conventional_segment.paddr = 0;
    m_largest_conventional_segment.length = 0;

    // Walk all entries to calculate total and largest conventional memory
    for (uint32_t i = 0; i < m_num_entries; ++i) {
        uint8_t* desc_ptr = m_efi_mmap_tag->efi_mmap + i * m_descr_size;
        efi_memory_descriptor* desc = reinterpret_cast<efi_memory_descriptor*>(desc_ptr);

        uint64_t length = desc->page_count * 4096; // Assuming 4KB pages

        // Add to total system memory
        m_total_system_memory += length;

        // Ignore non-conventional memory regions
        if (desc->type != 7) {
            continue;
        }

        // Add to total conventional memory
        m_total_conventional_memory += length;

        // Check if this is the largest segment
        if (length > m_largest_conventional_segment.length) {
            m_largest_conventional_segment.desc = desc;
            m_largest_conventional_segment.paddr = desc->physical_start;
            m_largest_conventional_segment.length = length;
        }
    }
}

// Iterator constructor
__PRIVILEGED_CODE
efi_memory_map::iterator::iterator(uint8_t* current, uint32_t descr_size, uint32_t index, uint32_t num_entries)
    : m_current(current), m_descr_size(descr_size), m_index(index), m_num_entries(num_entries) {
}

__PRIVILEGED_CODE
bool efi_memory_map::iterator::operator!=(const iterator& other) const {
    return m_index != other.m_index;
}

__PRIVILEGED_CODE
efi_memory_descriptor_wrapper efi_memory_map::iterator::operator*() const {
    efi_memory_descriptor* desc = reinterpret_cast<efi_memory_descriptor*>(m_current);
    efi_memory_descriptor_wrapper wrapper;
    wrapper.desc = desc;
    wrapper.paddr = desc->physical_start;
    wrapper.length = desc->page_count * 4096; // Assuming 4KB pages
    return wrapper;
}

__PRIVILEGED_CODE
efi_memory_map::iterator& efi_memory_map::iterator::operator++() {
    m_current += m_descr_size;
    m_index++;
    return *this;
}

__PRIVILEGED_CODE
efi_memory_map::iterator efi_memory_map::begin() const {
    uint8_t* start = m_efi_mmap_tag->efi_mmap;
    return iterator(start, m_descr_size, 0, m_num_entries);
}

__PRIVILEGED_CODE
efi_memory_map::iterator efi_memory_map::end() const {
    uint8_t* end = m_efi_mmap_tag->efi_mmap + m_num_entries * m_descr_size;
    return iterator(end, m_descr_size, m_num_entries, m_num_entries);
}

__PRIVILEGED_CODE
uint32_t efi_memory_map::get_num_entries() const {
    return m_num_entries;
}

__PRIVILEGED_CODE
uint64_t efi_memory_map::get_total_system_memory() const {
    return m_total_system_memory;
}

__PRIVILEGED_CODE
uint64_t efi_memory_map::get_total_conventional_memory() const {
    return m_total_conventional_memory;
}

__PRIVILEGED_CODE
efi_memory_descriptor_wrapper efi_memory_map::get_largest_conventional_segment() const {
    return m_largest_conventional_segment;
}

__PRIVILEGED_CODE
efi_memory_descriptor_wrapper efi_memory_map::find_segment_for_allocation_block(
    uint64_t min_address,
    uint64_t max_address,
    uint64_t size
) const {
    efi_memory_descriptor_wrapper largest_segment;
    largest_segment.desc = nullptr;
    largest_segment.paddr = 0;
    largest_segment.length = 0;

    // Iterate through all descriptors
    for (uint32_t i = 0; i < m_num_entries; ++i) {
        uint8_t* desc_ptr = m_efi_mmap_tag->efi_mmap + i * m_descr_size;
        efi_memory_descriptor* desc = reinterpret_cast<efi_memory_descriptor*>(desc_ptr);

        // Ignore non-conventional memory regions
        if (desc->type != 7) {
            continue;
        }

        uint64_t start = desc->physical_start;
        uint64_t length = desc->page_count * 4096; // Assuming 4KB pages
        uint64_t end = start + length;

        // Check if the segment overlaps with the specified range
        if (end <= min_address || start >= max_address) {
            continue;
        }

        // Clip the segment to the specified range
        uint64_t clipped_start = MAX(start, min_address);
        uint64_t clipped_end = MIN(end, max_address);
        uint64_t clipped_length = clipped_end - clipped_start;

        // Check if the clipped segment can fit the requested size
        if (clipped_length >= size && clipped_length > largest_segment.length) {
            largest_segment.desc = desc;
            largest_segment.paddr = clipped_start;
            largest_segment.length = clipped_length;
        }
    }

    return largest_segment;
}

} // namespace efi