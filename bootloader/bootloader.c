#include "elf_loader.h"
#include "memory_map.h"
#include "gop_setup.h"
#include "paging.h"

struct KernelEntryParams {
    UINT64 KernelPhysicalBase;

    struct {
        VOID*   Base;
        UINT64  Size;
        UINT32  Width;
        UINT32  Height;
        UINT32  PixelsPerScanline;
    } GraphicsFramebuffer;

    struct {
        VOID*   Base;
        UINT64  Size;
        UINT64  DescriptorSize;
        UINT64  DescriptorCount;
    } EfiMemoryMap;
};

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
        &EfiMemoryMap,
        &MemoryMapSize,
        &MemoryMapKey,
        &DescriptorSize,
        &TotalSystemMemory
    );

    if (EFI_ERROR(Status)) {
        return Status;
    }

    //
    // Now we have to create our own page table and do the following:
    //     1) Identity map all of the system memory
    //     2) Identity map the graphics output buffer
    //     3) Map the kernel to a higher half of the address space (base at 0xffffffff80000000...)
    //
    struct PageTable* PML4 = CreateIdentityMappedPageTable(
        TotalSystemMemory,
        (VOID*)GraphicsOutputProtocol->Mode->FrameBufferBase,
        (UINT64)GraphicsOutputProtocol->Mode->FrameBufferSize
    );
    if (PML4 == NULL) {
        Print(L"Error occured while creating initial page table\n\r");
        return EFIERR(-1);
    }

    // Map the kernel to the higher half
    MapKernelToHigherHalf(PML4, KernelPhysicalBase, KernelVirtualBase, KernelSize);

    Print(L"\n\r------ Page Table PML4 Created ------\n\r");
    Print(L"    Pages Allocated  : %llu\n\r", GetAllocatedPageCount());
    Print(L"    Page Table Size  : %llu KB\n\r\n\r", GetAllocatedMemoryCount() / 1024);

    // Read the memory map one more time to acquire the final memory map key
    Status = ReadMemoryMap(
        &EfiMemoryMap,
        &MemoryMapSize,
        &MemoryMapKey,
        &DescriptorSize,
        &TotalSystemMemory
    );
    
    if (EFI_ERROR(Status)) {
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
    __asm__ volatile("mov %0, %%cr3" : : "r"((UINT64)PML4));

    // Initialize params
    struct KernelEntryParams params;
    params.KernelPhysicalBase = (UINT64)KernelPhysicalBase;
    
    params.GraphicsFramebuffer.Base = (VOID*)GraphicsOutputProtocol->Mode->FrameBufferBase;
    params.GraphicsFramebuffer.Size = (UINT64)GraphicsOutputProtocol->Mode->FrameBufferSize;
    params.GraphicsFramebuffer.Width = (UINT64)GraphicsOutputProtocol->Mode->Info->HorizontalResolution;
    params.GraphicsFramebuffer.Height = (UINT64)GraphicsOutputProtocol->Mode->Info->VerticalResolution;
    params.GraphicsFramebuffer.PixelsPerScanline = (UINT64)GraphicsOutputProtocol->Mode->Info->PixelsPerScanLine;

    params.EfiMemoryMap.Base = (VOID*)EfiMemoryMap;
    params.EfiMemoryMap.Size = MemoryMapSize;
    params.EfiMemoryMap.DescriptorSize = DescriptorSize;
    params.EfiMemoryMap.DescriptorCount = MemoryMapSize / DescriptorSize;

    // Cast the physical entry point to a function pointer
    void (*KernelEntryPoint)(struct KernelEntryParams*) =
        ((__attribute__((sysv_abi)) void(*)(struct KernelEntryParams*))((UINT64)KernelEntry));

    // Transfer control to the kernel
    KernelEntryPoint(&params);

    return EFI_SUCCESS;
}
