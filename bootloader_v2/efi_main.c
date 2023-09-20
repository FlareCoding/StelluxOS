#include "loader.h"
#include "graphics.h"
#include "fonts.h"

typedef struct KernelEntryParams {
    Framebuffer_t*          GraphicsFramebuffer;
    psf1_font_t*            TextRenderingFont;
    EFI_MEMORY_DESCRIPTOR*  MemoryMap;
    UINTN                   MemoryMapSize;
    UINTN                   MemoryMapDescriptorSize;
} KernelEntryParams_t;

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS status;

    InitializeLib(ImageHandle, SystemTable);
    Print(L"Stardust Bootloader - V%u.%u DEBUG ON\n\r", 0, 1);

    // Load the kernel elf file from the filesystem
    EFI_FILE* KernelEfiFile = LoadFile(NULL, L"kernel.elf", ImageHandle, SystemTable);

    if (KernelEfiFile == NULL) {
        Print(L"Failed to load kernel.elf\n\r");
        return -1;
    }

    // Read the kernel file into an elf64 header
    Elf64_Ehdr KernelHeader;
    status = ReadKernelHeader(KernelEfiFile, &KernelHeader);
    if (EFI_ERROR(status)) {
        Print(L"Failed to read kernel into elf header\n\r");
        Print(L"Error code: %u\n\r", status);
        return status;
    }

    // Verify that the kernel is a valid ELF binary
    if (VerifyKernelHeader(&KernelHeader) != EFI_SUCCESS) {
        Print(L"Failed to verify kernel format\n\r");
        return -1;
    }

    Print(L"Kernel header successfully verified!\n\r");
    
    status = uefi_call_wrapper(KernelEfiFile->SetPosition, 2, KernelEfiFile, KernelHeader.e_phoff);
    if (EFI_ERROR(status)) {
        Print(L"Failed set position in the kernel efi file\n\r");
        Print(L"Error code: %u\n\r", status);
        return status;
    }

	// Program Headers
    Elf64_Phdr* PhHdrs = NULL;
    UINTN PhHdrsSize = KernelHeader.e_phnum * KernelHeader.e_phentsize;

    PhHdrs = AllocateZeroPool(PhHdrsSize);

    status = uefi_call_wrapper(KernelEfiFile->Read, 3, KernelEfiFile, &PhHdrsSize, PhHdrs);
    if (EFI_ERROR(status)) {
        Print(L"Failed to read into phheaders\n\r");
        Print(L"Error code: %u\n\r", status);
        return status;
    }

    // Iterate over every program header in the kernel ELF
    for (
        Elf64_Phdr* phdr = PhHdrs;
        (char*)phdr < (char*)PhHdrs + KernelHeader.e_phnum * KernelHeader.e_phentsize;
        phdr = (Elf64_Phdr*)((char*)phdr + KernelHeader.e_phentsize)
    ) {
        switch (phdr->p_type) {
            case PT_LOAD: {
                int pages = (phdr->p_memsz + 0x1000 - 1) / 0x1000;
                void* KernelAddr = (void*)phdr->p_paddr;

                status = uefi_call_wrapper(KernelEfiFile->SetPosition, 2, KernelEfiFile, phdr->p_offset);
                if (EFI_ERROR(status)) {
                    Print(L"Failed to update kernel read offset\n\r");
                    return -1;
                }

                UINTN PhdrFileSz = phdr->p_filesz;

                status = uefi_call_wrapper(KernelEfiFile->Read, 3, KernelEfiFile, &PhdrFileSz, KernelAddr);
                if (EFI_ERROR(status)) {
                    Print(L"Failed to read segment\n\r");
                    Print(L"Error code: %u\n\r", status);
                    return -1;
                }

                break;
            }
            default: break;
        }
    }

    Print(L"Kernel successfully loaded!\n\r");

	Framebuffer_t* framebuffer = InitGraphicsProtocol(SystemTable);
	Print(L"Base   : 0x%x\n\r", framebuffer->Base);
	Print(L"Size   : 0x%x\n\r", framebuffer->Size);
	Print(L"Width  :   %u\n\r", framebuffer->Width);
	Print(L"Height :   %u\n\r", framebuffer->Height);
	Print(L"PPSL   :   %u\n\r", framebuffer->PixelsPerScanline);

    psf1_font_t* KernelFont = LoadPSF1Font(NULL, L"zap-light16.psf", ImageHandle, SystemTable);
    if (KernelFont == NULL) {
        return -1;
    }
    
    Print(L"Loaded zap-light16.psf\n\rChar size: %u\n\r", KernelFont->Header->CharSize);

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

    KernelEntryParams_t kParams;
    kParams.GraphicsFramebuffer = framebuffer;
    kParams.TextRenderingFont = KernelFont;
    kParams.MemoryMap = MemoryMap;
    kParams.MemoryMapSize = MMapSize;
    kParams.MemoryMapDescriptorSize = DescriptorSize;

    // Exit EFI boot services
    uefi_call_wrapper(SystemTable->BootServices->ExitBootServices, 2, ImageHandle, MMapKey);

	void (*_kstart)(KernelEntryParams_t*) = ((__attribute__((sysv_abi)) void(*)(KernelEntryParams_t*)) KernelHeader.e_entry);
	_kstart(&kParams);

	// Should never be reached
    return EFI_SUCCESS;
}
