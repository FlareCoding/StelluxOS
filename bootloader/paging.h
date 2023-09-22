#ifndef PAGING_H
#define PAGING_H
#include "common.h"

#define PAGE_SIZE 0x1000

struct page_index_dict {
    uint64_t pt_lvl_4;      // Page Directory Pointer table index
    uint64_t pt_lvl_3;      // Page Directory table index
    uint64_t pt_lvl_2;      // Page Table index
    uint64_t pt_lvl_1;      // Page index inside the page table
};

struct page_table_entry {
    union
    {
        struct
        {
            uint64_t present                : 1;    // Must be 1, region invalid if 0.
            uint64_t read_write             : 1;    // If 0, writes not allowed.
            uint64_t user_supervisor        : 1;    // If 0, user-mode accesses not allowed.
            uint64_t page_write_through     : 1;    // Determines the memory type used to access the memory.
            uint64_t page_cache_disabled    : 1;    // Determines the memory type used to access the memory.
            uint64_t accessed               : 1;    // If 0, this entry has not been used for translation.
            uint64_t dirty                  : 1;    // If 0, the memory backing this page has not been written to.
            uint64_t page_access_type       : 1;    // Determines the memory type used to access the memory.
            uint64_t global                 : 1;    // If 1 and the PGE bit of CR4 is set, translations are global.
            uint64_t ignored2               : 3;
            uint64_t page_frame_number      : 36;   // The page frame number of the backing physical page.
            uint64_t reserved               : 4;
            uint64_t ignored3               : 7;
            uint64_t protection_key         : 4;    // If the PKE bit of CR4 is set, determines the protection key.
            uint64_t execute_disable        : 1;    // If 1, instruction fetches not allowed.
        };
        uint64_t value;
    };
} __attribute__((packed));

struct page_table {
    struct page_table_entry entries[512];
} __attribute__((aligned(PAGE_SIZE)));

uint64_t GetAllocatedMemoryCount();
uint64_t GetAllocatedPageCount();

void* krequest_page();

void _kvaddr_to_page_offsets(
    uint64_t addr,
    struct page_index_dict* dict
);

void kmemset(void* base, uint8_t val, uint64_t size);

void MapPages(void* vaddr, void* paddr, struct page_table* pml4);

#endif
