#include <boot/efimem.h>

namespace efi {
efi_memory_map::efi_memory_map(multiboot_tag_efi_mmap* efi_mmap_tag)
    : m_efi_mmap_tag(efi_mmap_tag), m_total_conventional_memory(0) {
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

        // Ignore non-conventional memory regions
        if (desc->type != 7) {
            continue;
        }

        uint64_t length = desc->page_count * 4096; // Assuming 4KB pages

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
efi_memory_map::iterator::iterator(uint8_t* current, uint32_t descr_size, uint32_t index, uint32_t num_entries)
    : m_current(current), m_descr_size(descr_size), m_index(index), m_num_entries(num_entries) {
}

bool efi_memory_map::iterator::operator!=(const iterator& other) const {
    return m_index != other.m_index;
}

efi_memory_descriptor_wrapper efi_memory_map::iterator::operator*() const {
    efi_memory_descriptor* desc = reinterpret_cast<efi_memory_descriptor*>(m_current);
    efi_memory_descriptor_wrapper wrapper;
    wrapper.desc = desc;
    wrapper.paddr = desc->physical_start;
    wrapper.length = desc->page_count * 4096; // Assuming 4KB pages
    return wrapper;
}

efi_memory_map::iterator& efi_memory_map::iterator::operator++() {
    m_current += m_descr_size;
    m_index++;
    return *this;
}

efi_memory_map::iterator efi_memory_map::begin() const {
    uint8_t* start = m_efi_mmap_tag->efi_mmap;
    return iterator(start, m_descr_size, 0, m_num_entries);
}

efi_memory_map::iterator efi_memory_map::end() const {
    uint8_t* end = m_efi_mmap_tag->efi_mmap + m_num_entries * m_descr_size;
    return iterator(end, m_descr_size, m_num_entries, m_num_entries);
}

uint32_t efi_memory_map::get_num_entries() const {
    return m_num_entries;
}

uint64_t efi_memory_map::get_total_conventional_memory() const {
    return m_total_conventional_memory;
}

efi_memory_descriptor_wrapper efi_memory_map::get_largest_conventional_segment() const {
    return m_largest_conventional_segment;
}
} // namespace efi
