#include "page.h"
#include "phys_addr_translation.h"
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

void mapPage(
    void* vaddr,
    void* paddr,
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
		pml4_entry->pageFrameNumber = reinterpret_cast<uint64_t>(pdpt) >> 12;
	} else {
		pdpt = (PageTable*)((uint64_t)pml4_entry->pageFrameNumber << 12);
	}

	pte_t* pdpt_entry = &pdpt->entries[pdptIndex];
	
	if (pdpt_entry->present == 0) {
		pdt = (PageTable*)__pa(pageFrameAllocator.requestFreePageZeroed());

		pdpt_entry->present = 1;
		pdpt_entry->readWrite = 1;
		pdpt_entry->pageFrameNumber = reinterpret_cast<uint64_t>(pdt) >> 12;
	} else {
		pdt = (PageTable*)((uint64_t)pdpt_entry->pageFrameNumber << 12);
	}

	pte_t* pdt_entry = &pdt->entries[pdtIndex];
	
	if (pdt_entry->present == 0) {
		pt = (PageTable*)__pa(pageFrameAllocator.requestFreePageZeroed());

		pdt_entry->present = 1;
		pdt_entry->readWrite = 1;
		pdt_entry->pageFrameNumber = reinterpret_cast<uint64_t>(pt) >> 12;
	} else {
		pt = (PageTable*)((uint64_t)pdt_entry->pageFrameNumber << 12);
	}

	pte_t* pte = &pt->entries[ptIndex];
	pte->present = 1;
	pte->readWrite = 1;
	pte->pageFrameNumber = reinterpret_cast<uint64_t>(paddr) >> 12;
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
    kprint("------ page_table_entry 0x%llx ------\n", pte);
    kprint("    present             : %i\n", (int)pte->present);
    kprint("    read_write          : %i\n", (int)pte->readWrite);
    kprint("    user_supervisor     : %i\n", (int)pte->userSupervisor);
    kprint("    page_write_through  : %i\n", (int)pte->pageWriteThrough);
    kprint("    page_cache_disabled : %i\n", (int)pte->pageCacheDisabled);
    kprint("    accessed            : %i\n", (int)pte->accessed);
    kprint("    dirty               : %i\n", (int)pte->dirty);
    kprint("    page_access_type    : %i\n", (int)pte->pageAccessType);
    kprint("    global              : %i\n", (int)pte->global);
    kprint("    page_frame_number   : 0x%llx\n", (uint64_t)pte->pageFrameNumber);
    kprint("    protection_key      : %i\n", (int)pte->protectionKey);
    kprint("    execute_disable     : %i\n", (int)pte->executeDisable);
}

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

void setCurrentTopLevelPageTable(PageTable* pml4) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(reinterpret_cast<uint64_t>(__pa(pml4))));
}
} // namespace paging
