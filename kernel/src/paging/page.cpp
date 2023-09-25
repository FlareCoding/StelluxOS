#include "page.h"
#include "phys_addr_translation.h"

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
		pdpt = (PageTable*)pageFrameAllocator.requestFreePageZeroed();

		pml4_entry->present = 1;
		pml4_entry->readWrite = 1;
		pml4_entry->pageFrameNumber = reinterpret_cast<uint64_t>(pdpt) >> 12;
	} else {
		pdpt = (PageTable*)((uint64_t)pml4_entry->pageFrameNumber << 12);
	}

	pte_t* pdpt_entry = &pdpt->entries[pdptIndex];
	
	if (pdpt_entry->present == 0) {
		pdt = (PageTable*)pageFrameAllocator.requestFreePageZeroed();

		pdpt_entry->present = 1;
		pdpt_entry->readWrite = 1;
		pdpt_entry->pageFrameNumber = reinterpret_cast<uint64_t>(pdt) >> 12;
	} else {
		pdt = (PageTable*)((uint64_t)pdpt_entry->pageFrameNumber << 12);
	}

	pte_t* pdt_entry = &pdt->entries[pdtIndex];
	
	if (pdt_entry->present == 0) {
		pt = (PageTable*)pageFrameAllocator.requestFreePageZeroed();

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
	uint64_t pml4Index, pdptIndex, pdtIndex, ptIndex;
	getPageTableIndicesFromVirtualAddress(
        reinterpret_cast<uint64_t>(vaddr),
        &pml4Index, &pdptIndex, &pdtIndex, &ptIndex
    );

	PageTable *pdpt = nullptr, *pdt = nullptr, *pt = nullptr;

	pte_t* pml4_entry = &pml4->entries[pml4Index];

	if (pml4_entry->present == 0) {
		return nullptr;
	} else {
		pdpt = (PageTable*)((uint64_t)pml4_entry->pageFrameNumber << 12);
	}

	pte_t* pdpt_entry = &pdpt->entries[pdptIndex];
	
	if (pdpt_entry->present == 0) {
		return nullptr;
	} else {
		pdt = (PageTable*)((uint64_t)pdpt_entry->pageFrameNumber << 12);
	}

	pte_t* pdt_entry = &pdt->entries[pdtIndex];
	
	if (pdt_entry->present == 0) {
		return nullptr;
	} else {
		pt = (PageTable*)((uint64_t)pdt_entry->pageFrameNumber << 12);
	}

	return &pt->entries[ptIndex];
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
