#include "loader.h"

EFI_FILE*
LoadFile(
    EFI_FILE* Dir,
    CHAR16* Path,
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE* SystemTable
) {
    EFI_FILE* File;
    EFI_STATUS status;

    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
    status = uefi_call_wrapper(
        SystemTable->BootServices->HandleProtocol,
        3,
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (void**)&LoadedImage
    );

    if (EFI_ERROR(status)) {
        Print(L"Failed to retrieve LoadedImage Protocol\n\r");
        return NULL;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Filesystem = NULL;
    status = uefi_call_wrapper(
        SystemTable->BootServices->HandleProtocol,
        3,
        LoadedImage->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (void**)&Filesystem
    );

    if (EFI_ERROR(status)) {
        Print(L"Failed to retrieve Filesystem Protocol\n\r");
        return NULL;
    }

    if (Dir == NULL) {
        status = uefi_call_wrapper(Filesystem->OpenVolume, 2, Filesystem, &Dir);

        if (EFI_ERROR(status)) {
            Print(L"Failed to open volume\n\r");
            return NULL;
        }
    }

    status = uefi_call_wrapper(Dir->Open, 5, Dir, &File, Path, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
    if (EFI_ERROR(status)) {
        return NULL;
    }

    return File;
}

EFI_FILE_INFO*
GetFileInfo(
    EFI_FILE* File
) {
    EFI_STATUS status;
    UINTN FileInfoSize = 0;
    EFI_FILE_INFO* FileInfo = NULL;

    if (File == NULL) {
        return NULL;
    }

    // Attempt to get info
    status = uefi_call_wrapper(File->GetInfo, 4, File, &gEfiFileInfoGuid, &FileInfoSize, NULL);

    // Normally, we expect a EFI_BUFFER_TOO_SMALL error after which
    // we allocate the pool and read file info again properly.
    if (status == EFI_BUFFER_TOO_SMALL) {
        // Allocate enough space for the file info buffer
        FileInfo = AllocateZeroPool(FileInfoSize);

        // Properly read the info again
        status = uefi_call_wrapper(File->GetInfo, 4, File, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);

        // If there is an error this time, then it's a real problem
        if (EFI_ERROR(status)) {
            Print(L"Failed to get file info\n\r");
            Print(L"Error code: %u\n\r", status);

            // Freeing the memory pool for the file info buffer
            FreePool(FileInfo);
            FileInfo = NULL;
        }
    }

    return FileInfo;
}

EFI_STATUS
ReadKernelHeader(
    IN EFI_FILE* KernelEfiFile,
    OUT Elf64_Ehdr* Hdr
) {
    EFI_FILE_INFO* FileInfo = GetFileInfo(KernelEfiFile);
    if (FileInfo == NULL) {
        Print(L"Failed to get kernel file info\n\r");
        return -1;
    }

    UINTN size = sizeof(Elf64_Ehdr);

    EFI_STATUS status = uefi_call_wrapper(KernelEfiFile->Read, 3, KernelEfiFile, &size, Hdr);
    if (EFI_ERROR(status)) {
        return status;
    }

    return EFI_SUCCESS;
}

EFI_STATUS
VerifyKernelHeader(
    Elf64_Ehdr* Hdr
) {
    if (CompareMem(&Hdr->e_ident[EI_MAG0], ELFMAG, SELFMAG) != 0 ||
        Hdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        Hdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        Hdr->e_type != ET_EXEC ||
        Hdr->e_machine != EM_X86_64 ||
        Hdr->e_version != EV_CURRENT) {
        return -1;
    }

    return EFI_SUCCESS;
}
