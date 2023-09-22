#include "elf_loader.h"
#include "memory_map.h"
#include "gop_setup.h"
#include "paging.h"

#define KERNEL_VIRTUAL_BASE 0xFFFF800000000000

struct KernelEntryParams {
    void* GopFramebufferBase;
    void* GopFramebufferSize;
    unsigned int GopFramebufferWidth;
    unsigned int GopFramebufferHeight;
    unsigned int GopPixelsPerScanLine;
};

struct page_table* create_pml4(UINT64 TotalSystemMemory, VOID* KernelPhysicalBase, UINT64 KernelSize, VOID* GopBufferBase, UINTN GopBufferSize) {
    // Allocate page tables
    struct page_table *pml4 = (struct page_table*)krequest_page();
    kmemset(pml4, 0, PAGE_SIZE);

    // Identity map the first 2GB of memory
    for (UINT64 i = 0; i < TotalSystemMemory; i += PAGE_SIZE) {
        MapPages((void*)i, (void*)i, pml4);
    }

    // Map the kernel to a higher half
    for (UINT64 i = 0; i < KernelSize; i += PAGE_SIZE) {
        void* paddr = (void*)(i + (UINT64)KernelPhysicalBase);
        void* vaddr = (void*)(i + (UINT64)KERNEL_VIRTUAL_BASE);

        MapPages(vaddr, paddr, pml4);
        Print(L"Mapping 0x%llx --> 0x%llx\n\r", vaddr, paddr);
    }

    // Identity mapping the GOP buffer
    for (UINT64 i = (UINT64)GopBufferBase; i < (UINT64)GopBufferBase + GopBufferSize; i += PAGE_SIZE) {
        MapPages((void*)i, (void*)i, pml4);
    }

    return pml4;
}

UINT64 GetTotalSystemMemory(
    EFI_MEMORY_DESCRIPTOR* MemoryMap,
    UINT64 Entries,
    UINT64 DescriptorSize
) {
    UINT64 TotalMemory = 0;

    for (UINT64 i = 0; i < Entries; ++i) {
        EFI_MEMORY_DESCRIPTOR* desc =
            (EFI_MEMORY_DESCRIPTOR*)((UINT64)MemoryMap + (i * DescriptorSize));

        TotalMemory += desc->NumberOfPages * PAGE_SIZE;
    }

    return TotalMemory;
}

EFI_STATUS LoadKernel(
    VOID** KernelPhysicalBase,
    VOID** KernelVirtualBase,
    UINT64* KernelSize,
    VOID** KernelEntry
) {
    EFI_STATUS Status;
    EFI_FILE* KernelRootDir;

    // Open the root directory of the volume
    Status = OpenRootDirectory(&KernelRootDir);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to open volume root directory\n\r");
        return Status;
    }

    // Load the ELF kernel into memory
    Status = LoadElfFile(
        KernelRootDir,
        L"kernel.elf",
        KernelEntry,
        KernelPhysicalBase,
        KernelVirtualBase,
        KernelSize
    );

    if (EFI_ERROR(Status)) {
        Print(L"Failed to load kernel into memory\n\r");
        return Status;
    }

    Print(L"Kernel Loaded:\n\r");
    Print(L"    Physical Base : 0x%llx\n\r", *KernelPhysicalBase);
    Print(L"    Virtual Base  : 0x%llx\n\r", *KernelVirtualBase);
    Print(L"    Size          : 0x%llx (%llu pages)\n\r", *KernelSize, *KernelSize / PAGE_SIZE);
    Print(L"    Entry         : 0x%llx\n\r\n\r", *KernelEntry);

    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS Status;    

    InitializeLib(ImageHandle, SystemTable);
    Print(L"Stellux Bootloader - V%u.%u DEBUG ON\n\r\n\r", 0, 1);

    VOID* EntryPoint = NULL;
    VOID* KernelBase = NULL;
    VOID* KernelVirtualBase = NULL;
    UINT64 KernelSize;
    Status = LoadKernel(&KernelBase, &KernelVirtualBase, &KernelSize, &EntryPoint);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    
    EFI_GRAPHICS_OUTPUT_PROTOCOL* Gop;
    Status = InitializeGOP(ImageHandle, &Gop);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to initialize GOP.\n\r");
        return Status;
    }

    // Initialize params
    struct KernelEntryParams params;
    params.GopFramebufferBase = (void*)Gop->Mode->FrameBufferBase;
    params.GopFramebufferSize = (void*)Gop->Mode->FrameBufferSize;
    params.GopFramebufferWidth = Gop->Mode->Info->HorizontalResolution;
    params.GopFramebufferHeight = Gop->Mode->Info->VerticalResolution;
    params.GopPixelsPerScanLine = Gop->Mode->Info->PixelsPerScanLine;

    // Cast the physical entry point to a function pointer
    void (*KernelEntryPoint)(struct KernelEntryParams*) = ((__attribute__((sysv_abi)) void(*)(struct KernelEntryParams*))((UINT64)EntryPoint));
    
    // Check if kernel load was successful
    if (KernelEntryPoint == NULL) {
        Print(L"Kernel entry point is NULL. Exiting.\n\r");
        return -1;
    }

    Print(L"Kernel entry is at 0x%llx\n\r", KernelEntryPoint);

    // Read system memory map
    EFI_MEMORY_DESCRIPTOR* MemoryMap = NULL;
    UINTN MMapSize, MMapKey;
    UINTN DescriptorSize;
    UINT32 DescriptorVersion;

    // First call will just give us the map size
    uefi_call_wrapper(
        SystemTable->BootServices->GetMemoryMap,
        5,
        &MMapSize,
        MemoryMap,
        &MMapKey,
        &DescriptorSize,
        &DescriptorVersion
    );

    // Allocate enough space for the memory map
    MemoryMap = AllocateZeroPool(MMapSize);

    // Actually read in the memory map
    uefi_call_wrapper(
        SystemTable->BootServices->GetMemoryMap,
        5,
        &MMapSize,
        MemoryMap,
        &MMapKey,
        &DescriptorSize,
        &DescriptorVersion
    );

    UINT64 DescriptorCount = MMapSize / DescriptorSize;
    UINT64 TotalSystemMemory = GetTotalSystemMemory(MemoryMap, DescriptorCount, DescriptorSize);
    Print(L"Total System Memory: 0x%llx\n\r", TotalSystemMemory);

    struct page_table* pml4 = create_pml4(TotalSystemMemory, KernelBase, KernelSize, (void*)Gop->Mode->FrameBufferBase, (UINTN)Gop->Mode->FrameBufferSize);
    if (pml4 == NULL) {
        Print(L"Error occured while creating page table\n\r");
        return -1;
    }
    Print(L"Page Table PML4 Created\n\r");
    Print(L"%llu pages allocated for the page table\n\r", GetAllocatedPageCount());
    Print(L"Page table size: %llu KB\n\r", GetAllocatedMemoryCount() / 1024);

    // Actually read in the memory map
    uefi_call_wrapper(
        SystemTable->BootServices->GetMemoryMap,
        5,
        &MMapSize,
        MemoryMap,
        &MMapKey,
        &DescriptorSize,
        &DescriptorVersion
    );

    // Exit boot services.
    Status = gBS->ExitBootServices(ImageHandle, MMapKey);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to exit boot services\n\r");
        return Status;
    }

    // Load PML4 address into CR3
    asm volatile ("mov %0, %%cr3" : : "r"((UINT64)pml4));

    // Transfer control to the kernel
    KernelEntryPoint(&params);

    return EFI_SUCCESS;
}
