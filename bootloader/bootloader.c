#include "elf_loader.h"
#include "memory_map.h"
#include "gop_setup.h"

struct KernelEntryParams {
    void* GopFramebufferBase;
    void* GopFramebufferSize;
    unsigned int GopFramebufferWidth;
    unsigned int GopFramebufferHeight;
    unsigned int GopPixelsPerScanLine;
};

EFI_STATUS ExitBootServicesSimple(EFI_HANDLE ImageHandle) {
    EFI_STATUS Status;
    UINTN MapSize, MapKey, DescriptorSize;
    UINT32 DescriptorVersion;
    EFI_MEMORY_DESCRIPTOR *MemoryMap;

    // Initial call to get the required buffer size
    MapSize = 0;
    Status = uefi_call_wrapper(BS->GetMemoryMap, 5, &MapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"Failed to get memory map size: %r\n", Status);
        return Status;
    }

    // Allocate space for the memory map
    Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, MapSize, (void**)&MemoryMap);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to allocate memory for memory map: %r\n", Status);
        return Status;
    }

    // Get the actual memory map
    Status = uefi_call_wrapper(BS->GetMemoryMap, 5, &MapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to get memory map: %r\n", Status);
        return Status;
    }

    // Exit boot services
    Status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, MapKey);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to exit boot services: %r\n", Status);
        return Status;
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
    void (*KernelEntryPoint)(struct KernelEntryParams*) = ((__attribute__((sysv_abi)) void(*)(struct KernelEntryParams*))EntryPoint);

    // Check if kernel load was successful
    if (KernelEntryPoint == NULL) {
        Print(L"Kernel entry point is NULL. Exiting.\n\r");
        return -1;
    }

    Print(L"Exiting boot services...\n\rKernel entry is at 0x%x\n\r", KernelEntryPoint);
    
    UINTN MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MapKey = 0;
    UINTN DescriptorSize = 0;
    UINT32 DescriptorVersion = 0;

    // Get the required buffer size for the memory map.
    Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

    // Actually allocate the buffer for the memory map.
    MemoryMapSize += 8 * DescriptorSize;
    Status = gBS->AllocatePool(EfiLoaderData, MemoryMapSize, (void**)&MemoryMap);

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

    // Transfer control to the kernel
    KernelEntryPoint(&params);

    return EFI_SUCCESS;
}
