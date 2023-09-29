#include "elf_loader.h"
#include "font_loader.h"
#include "memory_map.h"
#include "gop_setup.h"
#include "paging.h"

struct ElfSegmentInfo KernelElfSegments[MAX_LOADED_ELF_SEGMENTS] = { 0 };

struct KernelEntryParams {
    struct ElfSegmentInfo* KernelElfSegments;

    struct {
        VOID*   Base;
        UINT64  Size;
        UINT32  Width;
        UINT32  Height;
        UINT32  PixelsPerScanline;
    } GraphicsFramebuffer;

    struct PSF1_Font* TextRenderingFont;

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
        KernelSize,
        KernelElfSegments
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

    Print(L"------- GOP Framebuffer -------\n");
    Print(L"  Base: 0x%llx\n", (UINT64)GraphicsOutputProtocol->Mode->FrameBufferBase);
    Print(L"  Size: 0x%llx\n", (UINT64)GraphicsOutputProtocol->Mode->FrameBufferSize);
    Print(L"  Resolution: %llux%llu\n", (UINT64)GraphicsOutputProtocol->Mode->Info->HorizontalResolution, (UINT64)GraphicsOutputProtocol->Mode->Info->VerticalResolution);
    Print(L"  PixelsPerScanline: %llu\n", (UINT64)GraphicsOutputProtocol->Mode->Info->PixelsPerScanLine);
    Print(L"\n");

    // Load the text font file
    struct PSF1_Font* ZapLightFont = LoadPSF1Font(L"zap-light16.psf", ImageHandle, SystemTable);
    if (ZapLightFont == NULL) {
        Print(L"Failed to load zap-light16.psf font file\n\r");
        return EFIERR(-1);
    }
    
    Print(L"Loaded zap-light16.psf\n\rChar size: %u\n\r\n\r", ZapLightFont->Header->CharSize);

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
        Print(L"Error: %r\n", Status);
    }

    while (1) {
        int i = 0;
        i = i - 1;
        i = i * i;
    }
    return EFI_SUCCESS;
}
