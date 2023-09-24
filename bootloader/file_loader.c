#include "file_loader.h"

EFI_STATUS OpenRootDirectory(
    EFI_FILE** RootDir
) {
    EFI_STATUS Status;
    UINTN HandleCount;
    EFI_HANDLE* Handles;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem;

    // Locate handle(s) that support EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    Status = uefi_call_wrapper(
        BS->LocateHandleBuffer, 5, ByProtocol,
        &FileSystemProtocol, NULL, &HandleCount, &Handles
    );

    if (EFI_ERROR(Status)) {
        Print(L"Error locating file system: %r\n\r", Status);
        return Status;
    }

    // Usually, the file system is on the first handle, additional error checking is advised for production
    Status = uefi_call_wrapper(
        BS->HandleProtocol, 3, Handles[0],
        &FileSystemProtocol, (VOID**)&FileSystem
    );

    if (EFI_ERROR(Status)) {
        Print(L"Error obtaining file system: %r\n\r", Status);
        return Status;
    }

    // Open the root volume
    Status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, RootDir);
    if (EFI_ERROR(Status)) {
        Print(L"Error opening root volume: %r\n\r", Status);
        return Status;
    }

    return EFI_SUCCESS;
}

EFI_STATUS OpenFile(EFI_FILE* RootDir, CHAR16* FileName, EFI_FILE** File) {
    EFI_STATUS Status;
    Status = uefi_call_wrapper(
        RootDir->Open,
        5,
        RootDir,
        File,
        FileName,
        EFI_FILE_MODE_READ,
        0
    );
    
    if (EFI_ERROR(Status)) {
        Print(L"Error opening file: %r\n\r", Status);
        return Status;
    }

    return EFI_SUCCESS;
}
