#ifndef EFIMEM_H
#define EFIMEM_H
#include "multiboot2.h"

struct efi_memory_descriptor {
    multiboot_uint32_t type;
    multiboot_uint32_t reserved;
    multiboot_uint64_t physical_start;
    multiboot_uint64_t virtual_start;
    multiboot_uint64_t page_count;
    multiboot_uint64_t attribute;
};

#endif // EFIMEM_H
