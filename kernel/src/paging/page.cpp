#include "page.h"
#include "phys_addr_translation.h"
#include "tlb.h"
#include <kprint.h>

namespace paging {
PageTable* g_kernelRootPageTable;

void getPageTableIndicesFromVirtualAddress(
    uint64_t vaddr,
    uint64_t* ipml4,
    uint64_t* ipdpt,
    uint64_t* ipdt,
    uint64_t* ipt
) {
    vaddr >>= 12;
    *ipt = vaddr & 0x1ff;
    vaddr >>= 9;
    *ipdt = vaddr & 0x1ff;
    vaddr >>= 9;
    *ipdpt = vaddr & 0x1ff;
    vaddr >>= 9;
    *ipml4 = vaddr & 0x1ff;
}

__PRIVILEGED_CODE
PageTable* getCurrentTopLevelPageTable() {
    uint64_t cr3_value;
    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(cr3_value) // Output operand
        :                 // No input operand
        :                 // No clobbered register
    );
    
	void* pml4Vaddr = __va(reinterpret_cast<void*>(cr3_value));
	return static_cast<PageTable*>(pml4Vaddr);
}

__PRIVILEGED_CODE
void setCurrentTopLevelPageTable(PageTable* pml4) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(reinterpret_cast<uint64_t>(__pa(pml4))));
}

pte_t* getPml4Entry(void* vaddr, PageTable* pml4) {
	uint64_t index = (reinterpret_cast<uint64_t>(vaddr) >> 39) & 0x1ff;

	return &pml4->entries[index];
}

pte_t* getPdptEntry(void* vaddr, PageTable* pdpt) {
	uint64_t index = (reinterpret_cast<uint64_t>(vaddr) >> 30) & 0x1ff;

	return static_cast<pte_t*>(__va(&pdpt->entries[index]));
}

pte_t* getPdtEntry(void* vaddr, PageTable* pdt) {
	uint64_t index = (reinterpret_cast<uint64_t>(vaddr) >> 21) & 0x1ff;

	return static_cast<pte_t*>(__va(&pdt->entries[index]));
}

pte_t* getPteFromPageTable(void* vaddr, PageTable* pt) {
	uint64_t index = (reinterpret_cast<uint64_t>(vaddr) >> 12) & 0x1ff;

	return static_cast<pte_t*>(__va(&pt->entries[index]));
}

PageTable* getNextLevelPageTable(pte_t* entry) {
	uint64_t pageTablePhysicalAddr = static_cast<uint64_t>(entry->pageFrameNumber) << 12;
	return reinterpret_cast<PageTable*>(pageTablePhysicalAddr);
}

__PRIVILEGED_CODE
void mapPage(
    void* vaddr,
    void* paddr,
	uint8_t privilegeLevel,
    uint8_t attribs,
    PageTable* pml4,
    PageFrameAllocator& pageFrameAllocator
) {
    uint64_t pml4Index, pdptIndex, pdtIndex, ptIndex;
	getPageTableIndicesFromVirtualAddress(
        reinterpret_cast<uint64_t>(vaddr),
        &pml4Index, &pdptIndex, &pdtIndex, &ptIndex
    );

	PageTable *pdpt = nullptr, *pdt = nullptr, *pt = nullptr;

	pte_t* pml4_entry = &pml4->entries[pml4Index];

	if (pml4_entry->present == 0) {
		pdpt = (PageTable*)__pa(pageFrameAllocator.requestFreePageZeroed());

		pml4_entry->present = 1;
		pml4_entry->readWrite = 1;
		pml4_entry->userSupervisor = USERSPACE_PAGE;
		pml4_entry->pageFrameNumber = reinterpret_cast<uint64_t>(pdpt) >> 12;
	} else {
		pdpt = (PageTable*)((uint64_t)pml4_entry->pageFrameNumber << 12);
	}

	pte_t* pdpt_entry = &pdpt->entries[pdptIndex];
	
	if (pdpt_entry->present == 0) {
		pdt = (PageTable*)__pa(pageFrameAllocator.requestFreePageZeroed());

		pdpt_entry->present = 1;
		pdpt_entry->readWrite = 1;
		pdpt_entry->userSupervisor = USERSPACE_PAGE;
		pdpt_entry->pageFrameNumber = reinterpret_cast<uint64_t>(pdt) >> 12;
	} else {
		pdt = (PageTable*)((uint64_t)pdpt_entry->pageFrameNumber << 12);
	}

	pte_t* pdt_entry = &pdt->entries[pdtIndex];
	
	if (pdt_entry->present == 0) {
		pt = (PageTable*)__pa(pageFrameAllocator.requestFreePageZeroed());

		pdt_entry->present = 1;
		pdt_entry->readWrite = 1;
		pdt_entry->userSupervisor = USERSPACE_PAGE;
		pdt_entry->pageFrameNumber = reinterpret_cast<uint64_t>(pt) >> 12;
	} else {
		pt = (PageTable*)((uint64_t)pdt_entry->pageFrameNumber << 12);
	}

	pte_t* pte = &pt->entries[ptIndex];
	pte->present = 1;
	pte->readWrite = 1;
	pte->userSupervisor = privilegeLevel;
	pte->pageCacheDisabled = attribs & PAGE_ATTRIB_CACHE_DISABLED;
	pte->pageWriteThrough = attribs & PAGE_ATTRIB_WRITE_THROUGH;
	pte->pageAccessType = attribs & PAGE_ATTRIB_ACCESS_TYPE;
	pte->pageFrameNumber = reinterpret_cast<uint64_t>(paddr) >> 12;
}

__PRIVILEGED_CODE
void mapPages(
    void* vaddr,
    void* paddr,
    size_t pages,
    uint8_t privilegeLevel,
    uint8_t attribs,
    PageTable* pml4,
    PageFrameAllocator& pageFrameAllocator
) {
	// Create page mappings
	for (size_t i = 0; i < pages; i++) {
		uint64_t pageVaddr = (uint64_t)vaddr + PAGE_SIZE * i;
		uint64_t pagePaddr = (uint64_t)paddr + PAGE_SIZE * i;
		
		mapPage((void*)pageVaddr, (void*)pagePaddr, privilegeLevel, attribs, pml4, pageFrameAllocator);
	}

	// Flush the TLB
	flushTlbAll();
}

__PRIVILEGED_CODE
void changePageAttribs(void* vaddr, uint8_t attribs, PageTable* pml4) {
	pte_t* pte = getPteForAddr(vaddr, pml4);
	pte->pageCacheDisabled = attribs & PAGE_ATTRIB_CACHE_DISABLED;
	pte->pageWriteThrough = attribs & PAGE_ATTRIB_WRITE_THROUGH;
	pte->pageAccessType = attribs & PAGE_ATTRIB_ACCESS_TYPE;
	flushTlbPage(vaddr);
}

__PRIVILEGED_CODE
void markPageUncacheable(void* vaddr, PageTable* pml4) {
	pte_t* pte = getPteForAddr(vaddr, pml4);
	pte->pageCacheDisabled = 1;
	flushTlbPage(vaddr);
}

__PRIVILEGED_CODE
void markPageWriteThrough(void* vaddr, PageTable* pml4) {
	pte_t* pte = getPteForAddr(vaddr, pml4);
	pte->pageWriteThrough = 1;
	flushTlbPage(vaddr);
}

__PRIVILEGED_CODE
void markPageAccessType(void* vaddr, PageTable* pml4) {
	pte_t* pte = getPteForAddr(vaddr, pml4);
	pte->pageAccessType = 1;
	flushTlbPage(vaddr);
}

PageTableEntry* getPteForAddr(void* vaddr, PageTable* pml4) {
	pte_t* pml4Entry = getPml4Entry(vaddr, pml4);
	if (!pml4Entry->present) {
		return nullptr;
	}

	pte_t* pdptEntry = getPdptEntry(vaddr, getNextLevelPageTable(pml4Entry));
	if (!pdptEntry->present) {
		return nullptr;
	}

	pte_t* pdtEntry = getPdtEntry(vaddr, getNextLevelPageTable(pdptEntry));
	if (!pdtEntry->present) {
		return nullptr;
	}

	pte_t* pte = getPteFromPageTable(vaddr, getNextLevelPageTable(pdtEntry));
	if (!pte->present) {
		return nullptr;
	}

	return pte;
}

void dbgPrintPte(pte_t* pte) {
    kprintf("------ page_table_entry 0x%llx ------\n", pte);
    kprintf("    present             : %i\n", (int)pte->present);
    kprintf("    read_write          : %i\n", (int)pte->readWrite);
    kprintf("    user_supervisor     : %i\n", (int)pte->userSupervisor);
    kprintf("    page_write_through  : %i\n", (int)pte->pageWriteThrough);
    kprintf("    page_cache_disabled : %i\n", (int)pte->pageCacheDisabled);
    kprintf("    accessed            : %i\n", (int)pte->accessed);
    kprintf("    dirty               : %i\n", (int)pte->dirty);
    kprintf("    page_access_type    : %i\n", (int)pte->pageAccessType);
    kprintf("    global              : %i\n", (int)pte->global);
    kprintf("    page_frame_number   : 0x%llx\n", (uint64_t)pte->pageFrameNumber);
    kprintf("    protection_key      : %i\n", (int)pte->protectionKey);
    kprintf("    execute_disable     : %i\n", (int)pte->executeDisable);
}

PageTable* createUserspacePml4(
    PageTable* kernelPml4,
    PageFrameAllocator& allocator
) {
	PageTable* userPml4 = reinterpret_cast<PageTable*>(allocator.requestFreePageZeroed());
	
	// Copy only the kernel mappings
	userPml4->entries[511] = kernelPml4->entries[511];

	return userPml4;
}
} // namespace paging
