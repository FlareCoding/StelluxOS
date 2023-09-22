#ifndef PAGING_H
#define PAGING_H
#include "common.h"

struct page_index_dict {
    UINT64 pt_lvl_4;      // Page Directory Pointer table index
    UINT64 pt_lvl_3;      // Page Directory table index
    UINT64 pt_lvl_2;      // Page Table index
    UINT64 pt_lvl_1;      // Page index inside the page table
};

struct page_table_entry {
    union
    {
        struct
        {
            UINT64 present                : 1;    // Must be 1, region invalid if 0.
            UINT64 read_write             : 1;    // If 0, writes not allowed.
            UINT64 user_supervisor        : 1;    // If 0, user-mode accesses not allowed.
            UINT64 page_write_through     : 1;    // Determines the memory type used to access the memory.
            UINT64 page_cache_disabled    : 1;    // Determines the memory type used to access the memory.
            UINT64 accessed               : 1;    // If 0, this entry has not been used for translation.
            UINT64 dirty                  : 1;    // If 0, the memory backing this page has not been written to.
            UINT64 page_access_type       : 1;    // Determines the memory type used to access the memory.
            UINT64 global                 : 1;    // If 1 and the PGE bit of CR4 is set, translations are global.
            UINT64 ignored2               : 3;
            UINT64 page_frame_number      : 36;   // The page frame number of the backing physical page.
            UINT64 reserved               : 4;
            UINT64 ignored3               : 7;
            UINT64 protection_key         : 4;    // If the PKE bit of CR4 is set, determines the protection key.
            UINT64 execute_disable        : 1;    // If 1, instruction fetches not allowed.
        };
        UINT64 value;
    };
} __attribute__((packed));

struct page_table {
    struct page_table_entry entries[512];
} __attribute__((aligned(PAGE_SIZE)));

UINT64 GetAllocatedMemoryCount();
UINT64 GetAllocatedPageCount();

void* krequest_page();

void _kvaddr_to_page_offsets(
    UINT64 addr,
    struct page_index_dict* dict
);

void kmemset(void* base, uint8_t val, UINT64 size);

void MapPages(void* vaddr, void* paddr, struct page_table* pml4);

#endif
