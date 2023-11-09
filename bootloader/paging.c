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

void MapPage(VOID* vaddr, VOID* paddr, UINT8 leafpriv, struct PageTable* PML4) {
    struct PageLevelDictionary pdict;
	VirtualAddressToPageLevels((UINT64)vaddr, &pdict);

	struct PageTable *PDL3 = NULL, *PDL2 = NULL, *PageTable = NULL;

	struct PageTableEntry* PTE_L4 = &PML4->Entries[pdict.PTLevel4];

	UINT8 privilege = 0;

	// For all kernel pages in the higher half of the address space, privilege should be set to
	// usermode (PL=3) with the exception of pages explicitly marked as privileged.
	// This is due to the idea that the majority of the kernel should be operating in usermode with
	// the exception of very few privileged regions performing privileged instructions.
	if (pdict.PTLevel4 == 511) {
		privilege = 1;
	}

	if (PTE_L4->Present == 0) {
		PDL3 = (struct PageTable*)RequestPage();
		SetMem(PDL3, PAGE_SIZE, 0);

		PTE_L4->Present = 1;
		PTE_L4->ReadWrite = 1;
		PTE_L4->UserSupervisor = privilege;
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
		PTE_L3->UserSupervisor = privilege;
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
		PTE_L2->UserSupervisor = privilege;
		PTE_L2->PFN = (UINT64)PageTable >> 12;
	} else {
		PageTable = (struct PageTable*)((UINT64)PTE_L2->PFN << 12);
	}

	struct PageTableEntry* PTE_L1 = &PageTable->Entries[pdict.PTLevel1];
	PTE_L1->Present = 1;
	PTE_L1->ReadWrite = 1;
	PTE_L1->UserSupervisor = leafpriv;
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
        MapPage((VOID*)i, (VOID*)i, 0, PML4);
    }

	// Identity mapping the graphics output buffer
    for (
		UINT64 i = (UINT64)GraphicsOutputBufferBase;
		i < (UINT64)GraphicsOutputBufferBase + GraphicsOutputBufferSize;
		i += PAGE_SIZE
	) {
        MapPage((VOID*)i, (VOID*)i, 0, PML4);
    }

    return PML4;
}

INT64 FindElfSectionByVaddr(
    struct ElfSectionInfo* KernelElfSections,
    UINT64 KernelElfSectionCount,
	UINT64 KernelVirtualBase,
    UINT64 Vaddr
) {
    for (UINT64 i = 0; i < KernelElfSectionCount; ++i) {
        struct ElfSectionInfo section = KernelElfSections[i];
        UINT64 sectionStart = section.VirtualBase;
        UINT64 sectionEnd = section.VirtualBase + section.VirtualSize;

		if (sectionStart < KernelVirtualBase) {
			continue;
		}

        if (Vaddr >= sectionStart && Vaddr < sectionEnd) {
            return (INT64)i;
        }
    }

    return -1;  // Address not found in any section
}

void CreateHigherHalfMapping(
    struct PageTable* PML4,
	struct ElfSegmentInfo* KernelElfSegments,
    struct ElfSectionInfo* KernelElfSections,
	UINT64 KernelElfSectionCount,
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

		UINT8 LeafPrivilegeLevel = 0;
		
		if (vaddr >= KernelVirtualBase) {
			INT64 sectionIdx = FindElfSectionByVaddr(KernelElfSections, KernelElfSectionCount, KernelVirtualBase, vaddr);
			struct ElfSectionInfo section = KernelElfSections[sectionIdx];

			// Kernel pages are usermode by default unless explicitly marked as privileged
			LeafPrivilegeLevel = 1;

			if (sectionIdx != -1) {
				LeafPrivilegeLevel = !section.Privileged;
				
				// Logging
				// if (section.Privileged) {
				// 	Print(L"Mapping privileged kernel segment p:%llx -> v:%llx section:%a\n", paddr, vaddr, section.Name);
				// }
			}
		}

		// Check virtual address overflows
		if (vaddr == 0x0) {
			break;
		}

		MapPage((VOID*)vaddr, (VOID*)paddr, LeafPrivilegeLevel, PML4);
    }
}
