#include "elf_loader.h"
#include "memory_map.h"
#include "gop_setup.h"
#include "paging.h"

struct KernelEntryParams {
    void* GopFramebufferBase;
    void* GopFramebufferSize;
    unsigned int GopFramebufferWidth;
    unsigned int GopFramebufferHeight;
    unsigned int GopPixelsPerScanLine;
};

struct page_table* create_pml4(UINT64 TotalSystemMemory, VOID* KernelPhysicalBase, VOID* KernelVirtualBase, UINT64 KernelSize, VOID* GopBufferBase, UINTN GopBufferSize) {
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
        void* vaddr = (void*)(i + (UINT64)KernelVirtualBase);

        MapPages(vaddr, paddr, pml4);
        Print(L"Mapping 0x%llx --> 0x%llx\n\r", vaddr, paddr);
    }

    // Identity mapping the GOP buffer
    for (UINT64 i = (UINT64)GopBufferBase; i < (UINT64)GopBufferBase + GopBufferSize; i += PAGE_SIZE) {
        MapPages((void*)i, (void*)i, pml4);
    }

    return pml4;
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

EFI_STATUS ExitBootServices(
    EFI_HANDLE ImageHandle,
    UINTN MemoryMapKey
) {
    return gBS->ExitBootServices(ImageHandle, MemoryMapKey);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS Status;    

    // Initialize and print the header
    InitializeLib(ImageHandle, SystemTable);
    Print(L"Stellux Bootloader - V%u.%u DEBUG ON\n\r\n\r", 0, 1);

    // Load the ELF kernel into memory and retrieve the entry point
    VOID* KernelEntry = NULL;
    VOID* KernelPhysicalBase = NULL;
    VOID* KernelVirtualBase = NULL;
    UINT64 KernelSize;
    Status = LoadKernel(&KernelPhysicalBase, &KernelVirtualBase, &KernelSize, &KernelEntry);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    
    // Retrieve the graphics output protocol buffer
    EFI_GRAPHICS_OUTPUT_PROTOCOL* GraphicsOutputProtocol;
    Status = RetrieveGraphicsOutputProtocol(ImageHandle, &GraphicsOutputProtocol);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to initialize GOP.\n\r");
        return Status;
    }

    // Acquire information from the memory map
    EFI_MEMORY_DESCRIPTOR* EfiMemoryMap;
    UINTN MemoryMapSize, MemoryMapKey, DescriptorSize;
    UINT64 TotalSystemMemory;
    Status = ReadMemoryMap(
        SystemTable,
        &EfiMemoryMap,
        &MemoryMapSize,
        &MemoryMapKey,
        &DescriptorSize,
        &TotalSystemMemory
    );

    if (EFI_ERROR(Status)) {
        return Status;
    }

    struct page_table* pml4 = create_pml4(TotalSystemMemory, KernelPhysicalBase, KernelVirtualBase, KernelSize, (void*)GraphicsOutputProtocol->Mode->FrameBufferBase, (UINTN)GraphicsOutputProtocol->Mode->FrameBufferSize);
    if (pml4 == NULL) {
        Print(L"Error occured while creating page table\n\r");
        return -1;
    }
    Print(L"Page Table PML4 Created\n\r");
    Print(L"%llu pages allocated for the page table\n\r", GetAllocatedPageCount());
    Print(L"Page table size: %llu KB\n\r", GetAllocatedMemoryCount() / 1024);

    // Read the memory map one more time to acquire the final memory map key
    if (EFI_ERROR(ReadMemoryMap(
        SystemTable,
        &EfiMemoryMap,
        &MemoryMapSize,
        &MemoryMapKey,
        &DescriptorSize,
        &TotalSystemMemory
    ))) {
        Print(L"Failed to acquire final memory map key: %r\n\r", Status);
        return EFIERR(1);
    }

    // Exit boot services
    Status = ExitBootServices(ImageHandle, MemoryMapKey);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to exit boot services\n\r");
        return Status;
    }

    // Load PML4 address into CR3
    asm volatile ("mov %0, %%cr3" : : "r"((UINT64)pml4));

    // Initialize params
    struct KernelEntryParams params;
    params.GopFramebufferBase = (void*)GraphicsOutputProtocol->Mode->FrameBufferBase;
    params.GopFramebufferSize = (void*)GraphicsOutputProtocol->Mode->FrameBufferSize;
    params.GopFramebufferWidth = GraphicsOutputProtocol->Mode->Info->HorizontalResolution;
    params.GopFramebufferHeight = GraphicsOutputProtocol->Mode->Info->VerticalResolution;
    params.GopPixelsPerScanLine = GraphicsOutputProtocol->Mode->Info->PixelsPerScanLine;

    // Cast the physical entry point to a function pointer
    void (*KernelEntryPoint)(struct KernelEntryParams*) =
        ((__attribute__((sysv_abi)) void(*)(struct KernelEntryParams*))((UINT64)KernelEntry));

    // Transfer control to the kernel
    KernelEntryPoint(&params);

    return EFI_SUCCESS;
}
