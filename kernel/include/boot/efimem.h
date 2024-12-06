#ifndef EFIMEM_H
#define EFIMEM_H
#include <types.h>
#include "multiboot2.h"

namespace efi {
struct efi_memory_descriptor {
    multiboot_uint32_t type;
    multiboot_uint32_t reserved;
    multiboot_uint64_t physical_start;
    multiboot_uint64_t virtual_start;
    multiboot_uint64_t page_count;
    multiboot_uint64_t attribute;
};

struct efi_memory_descriptor_wrapper {
    efi_memory_descriptor* desc;
    uint64_t paddr;
    uint64_t length;
};

class efi_memory_map {
public:
    __PRIVILEGED_CODE efi_memory_map(multiboot_tag_efi_mmap* efi_mmap_tag);

    // Iterator class for range-based for loops
    class iterator {
    public:
        __PRIVILEGED_CODE iterator(uint8_t* current, uint32_t descr_size, uint32_t index, uint32_t num_entries);

        __PRIVILEGED_CODE bool operator!=(const iterator& other) const;
        __PRIVILEGED_CODE efi_memory_descriptor_wrapper operator*() const;
        __PRIVILEGED_CODE iterator& operator++();

    private:
        uint8_t* m_current;
        uint32_t m_descr_size;
        uint32_t m_index;
        uint32_t m_num_entries;
    };

    __PRIVILEGED_CODE iterator begin() const;
    __PRIVILEGED_CODE iterator end() const;

    __PRIVILEGED_CODE uint32_t get_num_entries() const;
    __PRIVILEGED_CODE uint64_t get_total_system_memory() const;
    __PRIVILEGED_CODE uint64_t get_total_conventional_memory() const;

    __PRIVILEGED_CODE efi_memory_descriptor_wrapper get_largest_conventional_segment() const;

    __PRIVILEGED_CODE efi_memory_descriptor_wrapper find_segment_for_allocation_block(
        uint64_t min_address,
        uint64_t max_address,
        uint64_t size
    ) const;

    __PRIVILEGED_CODE void print_memory_map();

private:
    multiboot_tag_efi_mmap* m_efi_mmap_tag;
    uint32_t m_descr_size;
    uint32_t m_num_entries;
    uint64_t m_total_system_memory;
    uint64_t m_total_conventional_memory;
    efi_memory_descriptor_wrapper m_largest_conventional_segment;
};
} // namespace efi

#endif // EFIMEM_H
