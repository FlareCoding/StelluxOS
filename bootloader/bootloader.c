#include "elf_loader.h"
#include "font_loader.h"
#include "memory_map.h"
#include "gop_setup.h"
#include "paging.h"

struct ElfSegmentInfo KernelElfSegments[MAX_LOADED_ELF_SEGMENTS] = { 0 };
UINT64 KernelElfSegmentCount = 0;

struct ElfSectionInfo KernelElfSections[MAX_LOADED_ELF_SEGMENTS] = { 0 };
UINT64 KernelElfSectionCount = 0;

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

    VOID* KernelStack;
    VOID* RSDP;
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
        KernelElfSegments,
        &KernelElfSegmentCount,
        KernelElfSections,
        &KernelElfSectionCount
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

#pragma pack(1)
typedef struct {
    CHAR8  Signature[8];
    UINT8  Checksum;
    CHAR8  OemId[6];
    UINT8  Revision;
    UINT32 RsdtAddress;
    UINT32 Length;
    UINT64 XsdtAddress;
    UINT8  ExtendedChecksum;
    UINT8  Reserved[3];
} EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER;
#pragma pack()

void PrintGuid(EFI_GUID *guid) {
    if (guid == NULL) {
        Print(L"NULL GUID pointer\n");
        return;
    }

    Print(L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
          guid->Data1,
          guid->Data2,
          guid->Data3,
          guid->Data4[0],
          guid->Data4[1],
          guid->Data4[2],
          guid->Data4[3],
          guid->Data4[4],
          guid->Data4[5],
          guid->Data4[6],
          guid->Data4[7]);
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
    
    EFI_PHYSICAL_ADDRESS KernelStackPhysicalAddress;
    gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &KernelStackPhysicalAddress);

    VOID* KernelStack = (VOID*)KernelStackPhysicalAddress;

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

    EFI_GUID Acpi2TableGuid = ACPI_20_TABLE_GUID;
    EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER* Rsdp = NULL;

    EFI_CONFIGURATION_TABLE* configTable = SystemTable->ConfigurationTable;
    for (UINTN index = 0; index < SystemTable->NumberOfTableEntries; index++) {
        if (CompareGuid(&configTable[index].VendorGuid, &Acpi2TableGuid) == 0) {
            if (strncmp("RSD PTR ", (char*)configTable[index].VendorTable, 8) == 0) {
                Rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER*)configTable[index].VendorTable;
                break;
            }
        }
    }

    // Acquire information from the memory map
    UINTN MemoryMapSize, MemoryMapKey, DescriptorSize;
    UINT64 TotalSystemMemory;
    EFI_MEMORY_DESCRIPTOR* EfiMemoryMap;

    EfiMemoryMap = ReadMemoryMap(
        &MemoryMapSize,
        &MemoryMapKey,
        &DescriptorSize,
        &TotalSystemMemory
    );

    if (EfiMemoryMap == NULL) {
        Print(L"[-] Failed to read memory map\n");
        return EFI_SUCCESS;
    }

    Print(L"Total system memory: %llu bytes (%llu GB)\n", TotalSystemMemory, TotalSystemMemory / 1024 / 1024 / 1024);

    //
    // Now we have to create our own page table and do the following:
    //     1) Identity map all of the system memory
    //     2) Identity map the graphics output buffer
    //     3) Map the kernel and the rest of physical memory to a higher
    //        half of the address space (kernel base at 0xffffffff80000000...)
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

    // Map the kernel and other memory to the higher half
    CreateHigherHalfMapping(PML4, KernelElfSegments, KernelElfSections, KernelElfSectionCount, TotalSystemMemory);

    Print(L"\n\r------ Page Table PML4 Created ------\n\r");
    Print(L"    Pages Allocated  : %llu\n\r", GetAllocatedPageCount());
    Print(L"    Page Table Size  : %llu KB\n\r", GetAllocatedMemoryCount() / 1024);
    Print(L"    PML4 Base        : 0x%llx\n\r\n\r", (UINT64)PML4);
    // Read the memory map one more time to acquire the final memory map key
    EfiMemoryMap = ReadMemoryMap(
        &MemoryMapSize,
        &MemoryMapKey,
        &DescriptorSize,
        &TotalSystemMemory
    );
    
    if (EFI_ERROR(Status)) {
        Print(L"Failed to acquire final memory map key: %r\n\r", Status);
        return EFIERR(1);
    }

    // Exit boot services to take full control of the system from UEFI
    Status = gBS->ExitBootServices(ImageHandle, MemoryMapKey);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to exit boot services\n\r");
        return Status;
    }

    // Load PML4 address into CR3
    __asm__ volatile("mov %0, %%cr3" : : "r"((UINT64)PML4));

    // Calculate kernel's higher half offset to convert physical addresses to virtual
    UINT64 KernelAddressSpaceOffset = (UINT64)KernelVirtualBase - (UINT64)KernelPhysicalBase;

    // Initialize params
    struct KernelEntryParams params;
    params.KernelElfSegments = (VOID*)((UINT64)KernelElfSegments + KernelAddressSpaceOffset);
    
    params.GraphicsFramebuffer.Base = (VOID*)(GraphicsOutputProtocol->Mode->FrameBufferBase);
    // Since the graphics buffer can get placed too high up in memory on high RAM systems,
    // it cannot be properly mapped to the higher half, so we convert its base to a higher half
    // address only if the conversion is possible and successful.
    if (((UINT64)params.GraphicsFramebuffer.Base + KernelAddressSpaceOffset) > (UINT64)KernelVirtualBase) {
        params.GraphicsFramebuffer.Base = (VOID*)((UINT64)params.GraphicsFramebuffer.Base + KernelAddressSpaceOffset);
    }

    params.GraphicsFramebuffer.Size = (UINT64)GraphicsOutputProtocol->Mode->FrameBufferSize;
    params.GraphicsFramebuffer.Width = (UINT64)GraphicsOutputProtocol->Mode->Info->HorizontalResolution;
    params.GraphicsFramebuffer.Height = (UINT64)GraphicsOutputProtocol->Mode->Info->VerticalResolution;
    params.GraphicsFramebuffer.PixelsPerScanline = (UINT64)GraphicsOutputProtocol->Mode->Info->PixelsPerScanLine;
    params.TextRenderingFont = (VOID*)((UINT64)ZapLightFont + KernelAddressSpaceOffset);

    params.EfiMemoryMap.Base = (VOID*)((UINT64)EfiMemoryMap + KernelAddressSpaceOffset);
    params.EfiMemoryMap.Size = MemoryMapSize;
    params.EfiMemoryMap.DescriptorSize = DescriptorSize;
    params.EfiMemoryMap.DescriptorCount = MemoryMapSize / DescriptorSize;

    params.KernelStack = (VOID*)((UINT64)KernelStack + KernelAddressSpaceOffset);
    params.RSDP = (VOID*)((UINT64)Rsdp + KernelAddressSpaceOffset);

    // Cast the physical entry point to a function pointer
    void (*KernelEntryPoint)(struct KernelEntryParams*) =
        ((__attribute__((sysv_abi)) void(*)(struct KernelEntryParams*))((UINT64)KernelEntry));

    // Transfer control to the kernel
    KernelEntryPoint(&params);

    return EFI_SUCCESS;
}
