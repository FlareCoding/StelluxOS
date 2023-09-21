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

struct page_table* create_pml4(VOID* KernelBase, VOID* GopBufferBase, UINTN GopBufferSize) {
    // Allocate page tables
    struct page_table *pml4 = (struct page_table*)krequest_page();
    kmemset(pml4, 0, PAGE_SIZE);

    // Identity map the first 2GB of memory
    for (uint64_t i = 0; i < 0x80000000; i += PAGE_SIZE) {
        MapPages((void*)i, (void*)i, pml4);
    }

    // Map the kernel to a higher half
    for (uint64_t i = 0; i < 10 * PAGE_SIZE; i += PAGE_SIZE) {
        void* paddr = (void*)(i + (uint64_t)KernelBase);
        void* vaddr = (void*)(i + (uint64_t)KERNEL_VIRTUAL_BASE);

        MapPages(vaddr, paddr, pml4);
        Print(L"Mapping 0x%llx --> 0x%llx\n\r", vaddr, paddr);
    }

    // Identity mapping the GOP buffer
    for (uint64_t i = (uint64_t)GopBufferBase; i < (uint64_t)GopBufferBase + GopBufferSize; i += PAGE_SIZE) {
        MapPages((void*)i, (void*)i, pml4);
    }

    return pml4;
}

EFI_STATUS ExitUEFIBootServices(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_MEMORY_DESCRIPTOR *memoryMap = NULL;
    UINTN mapSize = 0, mapKey = 0, descriptorSize = 0;
    UINT32 descriptorVersion = 0;

    // Step 1: Get the memory map size
    status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
                               &mapSize, memoryMap, &mapKey, &descriptorSize, &descriptorVersion);

    if (status == EFI_BUFFER_TOO_SMALL) {
        // Allocate buffer for the memory map
        mapSize += 2 * descriptorSize;  // Add some extra space to be sure
        status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                                   EfiLoaderData, mapSize, (void**)&memoryMap);

        if (EFI_ERROR(status)) {
            Print(L"Failed to allocate memory for memory map: %r\n", status);
            return status;
        }

        // Step 2: Get the memory map
        status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
                                   &mapSize, memoryMap, &mapKey, &descriptorSize, &descriptorVersion);

        if (EFI_ERROR(status)) {
            Print(L"Failed to get memory map: %r\n", status);
            return status;
        }
    }

    // Step 3: Exit Boot Services
    status = uefi_call_wrapper(SystemTable->BootServices->ExitBootServices, 2, ImageHandle, mapKey);

    if (EFI_ERROR(status)) {
        Print(L"Failed to exit boot services: %r\n", status);
        return status;
    }

    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    Print(L"Stellux Bootloader - V%u.%u DEBUG ON\n\r", 0, 1);

    EFI_STATUS Status;
    EFI_HANDLE* Handles;
    UINTN HandleCount;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem;

    // Locate handle(s) that support EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    Status = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol,
                &FileSystemProtocol, NULL, &HandleCount, &Handles);

    if (EFI_ERROR(Status)) {
        Print(L"Error locating file system: %r\n\r", Status);
        return Status;
    }

    // Usually, the file system is on the first handle, additional error checking is advised for production
    Status = uefi_call_wrapper(BS->HandleProtocol, 3, Handles[0],
                &FileSystemProtocol, (VOID**)&FileSystem);

    if (EFI_ERROR(Status)) {
        Print(L"Error obtaining file system: %r\n\r", Status);
        return Status;
    }

    // Open the root volume
    EFI_FILE* RootDir;
    Status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &RootDir);
    if (EFI_ERROR(Status)) {
        Print(L"Error opening root volume: %r\n\r", Status);
        return Status;
    }

    VOID* EntryPoint = NULL;
    VOID* KernelBase = NULL;
    Status = LoadElfKernel(RootDir, L"kernel.elf", &EntryPoint, &KernelBase);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to load kernel.\n\r");
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
    void (*KernelEntryPoint)(struct KernelEntryParams*) = ((__attribute__((sysv_abi)) void(*)(struct KernelEntryParams*))((uint64_t)EntryPoint));
    
    // Check if kernel load was successful
    if (KernelEntryPoint == NULL) {
        Print(L"Kernel entry point is NULL. Exiting.\n\r");
        return -1;
    }

    Print(L"Kernel entry is at 0x%llx\n\r", KernelEntryPoint);

    struct page_table* pml4 = create_pml4(KernelBase, (void*)Gop->Mode->FrameBufferBase, (UINTN)Gop->Mode->FrameBufferSize);
    if (pml4 == NULL) {
        Print(L"Error occured while creating page table\n\r");
        return -1;
    }
    Print(L"Page Table PML4 Created\n\r");
    
    UINTN MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MapKey = 0;
    UINTN DescriptorSize = 0;
    UINTN DescriptorCount = 0;
    UINT32 DescriptorVersion = 0;
    UINT64 TotalMemory = 0;

    // Get the required buffer size for the memory map.
    Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

    // Actually allocate the buffer for the memory map.
    MemoryMapSize += 8 * DescriptorSize;
    Status = gBS->AllocatePool(EfiLoaderData, MemoryMapSize, (void**)&MemoryMap);

    DescriptorCount = MemoryMapSize / DescriptorSize;
    for (uint64_t i = 0; i < DescriptorCount; ++i) {
        EFI_MEMORY_DESCRIPTOR* Descriptor =
            (EFI_MEMORY_DESCRIPTOR*)((uint64_t)MemoryMap + (i * DescriptorSize));

        TotalMemory += Descriptor->NumberOfPages * PAGE_SIZE;
    }

    // Get the actual memory map.
    Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (EFI_ERROR(Status)) {
        // Handle error
        return Status;
    }

    // StelluxMemoryDescriptor* MemoryMap;
    // UINTN MemoryMapSize = 0;
    // UINTN MapKey = 0;
    // Status = RetrieveMemoryMap(&MemoryMap, &MemoryMapSize, &MapKey);
    // if (EFI_ERROR(Status)) {
    //     Print(L"Failed to read memory map: %r\n\r", Status);
    //     return Status;
    // }

    // Exit boot services.
    Status = gBS->ExitBootServices(ImageHandle, MapKey);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to exit boot services\n\r");
        return Status;
    }

    // Load PML4 address into CR3
    asm volatile ("mov %0, %%cr3" : : "r"((uint64_t)pml4));

    // Transfer control to the kernel
    KernelEntryPoint(&params);

    return EFI_SUCCESS;
}
