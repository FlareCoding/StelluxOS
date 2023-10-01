#ifndef PAGING_H
#define PAGING_H
#include "common.h"
#include "elf_loader.h"

struct PageLevelDictionary {
    UINT64 PTLevel4;      // Page Directory Pointer table index
    UINT64 PTLevel3;      // Page Directory table index
    UINT64 PTLevel2;      // Page Table index
    UINT64 PTLevel1;      // Page index inside the page table
};

struct PageTableEntry {
    union
    {
        struct
        {
            UINT64 Present                : 1;    // Must be 1, region invalid if 0.
            UINT64 ReadWrite              : 1;    // If 0, writes not allowed.
            UINT64 UserSupervisor         : 1;    // If 0, user-mode accesses not allowed.
            UINT64 PageWriteThrough       : 1;    // Determines the memory type used to access the memory.
            UINT64 PageCacheDisabled      : 1;    // Determines the memory type used to access the memory.
            UINT64 Accessed               : 1;    // If 0, this entry has not been used for translation.
            UINT64 Dirty                  : 1;    // If 0, the memory backing this page has not been written to.
            UINT64 PageAccessType         : 1;    // Determines the memory type used to access the memory.
            UINT64 Global                 : 1;    // If 1 and the PGE bit of CR4 is set, translations are global.
            UINT64 Ignored2               : 3;
            UINT64 PFN                    : 36;   // The page frame number of the backing physical page.
            UINT64 Reserved               : 4;
            UINT64 Ignored3               : 7;
            UINT64 PK                     : 4;    // If the PKE bit of CR4 is set, determines the protection key.
            UINT64 EXD                    : 1;    // If 1, instruction fetches not allowed.
        };
        UINT64 Value;
    };
} __attribute__((packed));

struct PageTable {
    struct PageTableEntry Entries[512];
} __attribute__((aligned(PAGE_SIZE)));

UINT64 GetAllocatedMemoryCount();
UINT64 GetAllocatedPageCount();

VOID* RequestPage();

void VirtualAddressToPageLevels(
    UINT64 addr,
    struct PageLevelDictionary* dict
);

void MapPages(VOID* vaddr, VOID* paddr, struct PageTable* pml4);

// Returns PML4 (top level page table)
struct PageTable* CreateIdentityMappedPageTable(
    UINT64 TotalSystemMemory,
    VOID* GraphicsOutputBufferBase,
    UINT64 GraphicsOutputBufferSize
);

void CreateHigherHalfMapping(
    struct PageTable* PML4,
    struct ElfSegmentInfo* KernelElfSegments,
    UINT64 TotalSystemMemory
);

#endif
