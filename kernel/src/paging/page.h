#ifndef PAGE_H
#define PAGE_H
#include "page_frame_allocator.h"

#define PAGE_TABLE_ENTRIES 512

#define USERSPACE_PAGE  1
#define KERNEL_PAGE     0

#define PAGE_ATTRIB_CACHE_DISABLED 0x01  // Bit 0
#define PAGE_ATTRIB_WRITE_THROUGH  0x02  // Bit 1
#define PAGE_ATTRIB_ACCESS_TYPE    0x04  // Bit 2

namespace paging {
typedef struct PageTableEntry {
    union
    {
        struct
        {
            uint64_t present              : 1;    // Must be 1, region invalid if 0.
            uint64_t readWrite            : 1;    // If 0, writes not allowed.
            uint64_t userSupervisor       : 1;    // If 0, user-mode accesses not allowed.
            uint64_t pageWriteThrough     : 1;    // Determines the memory type used to access the memory.
            uint64_t pageCacheDisabled    : 1;    // Determines the memory type used to access the memory.
            uint64_t accessed             : 1;    // If 0, this entry has not been used for translation.
            uint64_t dirty                : 1;    // If 0, the memory backing this page has not been written to.
            uint64_t pageAccessType       : 1;    // Determines the memory type used to access the memory.
            uint64_t global               : 1;    // If 1 and the PGE bit of CR4 is set, translations are global.
            uint64_t ignored2             : 3;
            uint64_t pageFrameNumber      : 36;   // The page frame number of the backing physical page.
            uint64_t reserved             : 4;
            uint64_t ignored3             : 7;
            uint64_t protectionKey        : 4;    // If the PKE bit of CR4 is set, determines the protection key.
            uint64_t executeDisable       : 1;    // If 1, instruction fetches not allowed.
        };
        uint64_t value;
    };
} __attribute__((packed)) pte_t;

struct PageTable {
    pte_t entries[PAGE_TABLE_ENTRIES];
} __attribute__((aligned(PAGE_SIZE)));

static __force_inline__ void* pageAlignAddress(void* addr) {
    return (void*)((reinterpret_cast<uint64_t>(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
}

__PRIVILEGED_CODE
PageTable* getCurrentTopLevelPageTable();

__PRIVILEGED_CODE
void setCurrentTopLevelPageTable(PageTable* pml4);

__PRIVILEGED_CODE
void mapPage(
    void* vaddr,
    void* paddr,
    uint8_t privilegeLevel,
    uint8_t attribs,
    PageTable* pml4,
    PageFrameAllocator& pageFrameAllocator = getGlobalPageFrameAllocator()
);

__PRIVILEGED_CODE
void mapPages(
    void* vaddr,
    void* paddr,
    size_t pages,
    uint8_t privilegeLevel,
    uint8_t attribs,
    PageTable* pml4,
    PageFrameAllocator& pageFrameAllocator = getGlobalPageFrameAllocator()
);

__PRIVILEGED_CODE
void changePageAttribs(void* vaddr, uint8_t attribs, PageTable* pml4 = getCurrentTopLevelPageTable());

__PRIVILEGED_CODE
void markPageUncacheable(void* vaddr, PageTable* pml4 = getCurrentTopLevelPageTable());

__PRIVILEGED_CODE
void markPageWriteThrough(void* vaddr, PageTable* pml4 = getCurrentTopLevelPageTable());

__PRIVILEGED_CODE
void markPageAccessType(void* vaddr, PageTable* pml4 = getCurrentTopLevelPageTable());

pte_t* getPml4Entry(void* vaddr, PageTable* pml4);
pte_t* getPdptEntry(void* vaddr, PageTable* pdpt);
pte_t* getPdtEntry(void* vaddr, PageTable* pdt);
pte_t* getPteFromPageTable(void* vaddr, PageTable* pt);

PageTable* getNextLevelPageTable(pte_t* entry);

PageTableEntry* getPteForAddr(void* vaddr, PageTable* pml4);

void dbgPrintPte(pte_t* pte);

__PRIVILEGED_CODE
void setBlessedKernelAsid(PageTable* pml4);

__PRIVILEGED_CODE
bool isKernelAsid();

PageTable* createUserspacePml4(
    PageTable* kernelPml4,
    PageFrameAllocator& allocator = getGlobalPageFrameAllocator()
);

extern PageTable* g_kernelRootPageTable;
} // namespace paging
#endif
