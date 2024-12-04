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
    efi_memory_map(multiboot_tag_efi_mmap* efi_mmap_tag);

    // Iterator class for range-based for loops
    class iterator {
    public:
        iterator(uint8_t* current, uint32_t descr_size, uint32_t index, uint32_t num_entries);

        bool operator!=(const iterator& other) const;
        efi_memory_descriptor_wrapper operator*() const;
        iterator& operator++();

    private:
        uint8_t* m_current;
        uint32_t m_descr_size;
        uint32_t m_index;
        uint32_t m_num_entries;
    };

    iterator begin() const;
    iterator end() const;

    uint32_t get_num_entries() const;
    uint64_t get_total_conventional_memory() const;
    efi_memory_descriptor_wrapper get_largest_conventional_segment() const;

private:
    multiboot_tag_efi_mmap* m_efi_mmap_tag;
    uint32_t m_descr_size;
    uint32_t m_num_entries;
    uint64_t m_total_conventional_memory;
    efi_memory_descriptor_wrapper m_largest_conventional_segment;
};
} // namespace efi

#endif // EFIMEM_H
