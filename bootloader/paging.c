#include "paging.h"

UINT64 GlobalAllocatedMemory = 0;
UINT64 GlobalAllocatedPageCount = 0;

UINT64 GetAllocatedMemoryCount() {
	return GlobalAllocatedMemory;
}

UINT64 GetAllocatedPageCount() {
	return GlobalAllocatedPageCount;
}

VOID* RequestPage() {
    EFI_PHYSICAL_ADDRESS page;
    EFI_STATUS status = uefi_call_wrapper(
        BS->AllocatePages, 4,
        AllocateAnyPages,
        EfiLoaderData,
        1,
        &page
    );

    if (EFI_ERROR(status)) {
        Print(L"Could not allocate page: %r\n", status);
        return NULL;
    }

	GlobalAllocatedMemory += PAGE_SIZE;
	GlobalAllocatedPageCount++;
    return (VOID*)page;
}

void VirtualAddressToPageLevels(
    UINT64 addr,
    struct PageLevelDictionary* dict
) {
    addr >>= 12;
    dict->PTLevel1 = addr & 0x1ff;
    addr >>= 9;
    dict->PTLevel2 = addr & 0x1ff;
    addr >>= 9;
    dict->PTLevel3 = addr & 0x1ff;
    addr >>= 9;
    dict->PTLevel4 = addr & 0x1ff;
}

void MapPages(VOID* vaddr, VOID* paddr, struct PageTable* PML4) {
    struct PageLevelDictionary pdict;
	VirtualAddressToPageLevels((UINT64)vaddr, &pdict);

	struct PageTable *PDL3 = NULL, *PDL2 = NULL, *PageTable = NULL;

	struct PageTableEntry* PTE_L4 = &PML4->Entries[pdict.PTLevel4];

	if (PTE_L4->Present == 0) {
		PDL3 = (struct PageTable*)RequestPage();
		SetMem(PDL3, PAGE_SIZE, 0);

		PTE_L4->Present = 1;
		PTE_L4->ReadWrite = 1;
		PTE_L4->PFN = (UINT64)PDL3 >> 12;
	} else {
		PDL3 = (struct PageTable*)((UINT64)PTE_L4->PFN << 12);
	}

	struct PageTableEntry* PTE_L3 = &PDL3->Entries[pdict.PTLevel3];
	
	if (PTE_L3->Present == 0) {
		PDL2 = (struct PageTable*)RequestPage();
		SetMem(PDL2, PAGE_SIZE, 0);

		PTE_L3->Present = 1;
		PTE_L3->ReadWrite = 1;
		PTE_L3->PFN = (UINT64)PDL2 >> 12;
	} else {
		PDL2 = (struct PageTable*)((UINT64)PTE_L3->PFN << 12);
	}

	struct PageTableEntry* PTE_L2 = &PDL2->Entries[pdict.PTLevel2];
	
	if (PTE_L2->Present == 0) {
		PageTable = (struct PageTable*)RequestPage();
		SetMem(PageTable, PAGE_SIZE, 0);

		PTE_L2->Present = 1;
		PTE_L2->ReadWrite = 1;
		PTE_L2->PFN = (UINT64)PageTable >> 12;
	} else {
		PageTable = (struct PageTable*)((UINT64)PTE_L2->PFN << 12);
	}

	struct PageTableEntry* PTE_L1 = &PageTable->Entries[pdict.PTLevel1];
	PTE_L1->Present = 1;
	PTE_L1->ReadWrite = 1;
	PTE_L1->PFN = (UINT64)paddr >> 12;
}

struct PageTable* CreateIdentityMappedPageTable(
	UINT64 TotalSystemMemory,
    VOID* GraphicsOutputBufferBase,
    UINT64 GraphicsOutputBufferSize
) {
	// Allocate page tables
    struct PageTable* PML4 = (struct PageTable*)RequestPage();
    SetMem(PML4, PAGE_SIZE, 0);

    // Identity map all system memory
    for (UINT64 i = 0; i < TotalSystemMemory; i += PAGE_SIZE) {
        MapPages((VOID*)i, (VOID*)i, PML4);
    }

	// Identity mapping the graphics output buffer
    for (
		UINT64 i = (UINT64)GraphicsOutputBufferBase;
		i < (UINT64)GraphicsOutputBufferBase + GraphicsOutputBufferSize;
		i += PAGE_SIZE
	) {
        MapPages((VOID*)i, (VOID*)i, PML4);
    }

    return PML4;
}

void CreateHigherHalfMapping(
    struct PageTable* PML4,
    struct ElfSegmentInfo* KernelElfSegments,
	VOID** KernelStack,
	UINT64 TotalSystemMemory
) {
	UINT64 KernelPhysicalBase = (UINT64)KernelElfSegments[0].PhysicalBase;
	UINT64 KernelVirtualBase = (UINT64)KernelElfSegments[0].VirtualBase;
	
	// Calculate the offset between virtual and physical addresses for kernel
    UINT64 Offset = KernelVirtualBase - KernelPhysicalBase;

    for (UINT64 i = 0; i < TotalSystemMemory; i += PAGE_SIZE) {
		// Calculate the corresponding higher-half virtual address
		UINT64 paddr = i;
		UINT64 vaddr = paddr + Offset;

		// Check virtual address overflows
		if (vaddr == 0x0) {
			break;
		}

		MapPages((VOID*)vaddr, (VOID*)paddr, PML4);
    }

	// Map the kernel stack
	MapPages((VOID*)((UINT64)*KernelStack + Offset), KernelStack, PML4);

	// Update the caller's kernel stack address to be in the kernel's address space
	*KernelStack = (VOID*)((UINT64)*KernelStack + Offset);
}
