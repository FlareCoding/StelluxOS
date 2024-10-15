#include "file_loader.h"

EFI_STATUS OpenRootDirectory(EFI_FILE** RootDir) {
    EFI_STATUS Status;
    UINTN HandleCount;
    EFI_HANDLE* Handles;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem;
    EFI_FILE* TempRootDir;
    EFI_FILE* TempFile;

    // Locate handle(s) that support EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    Status = uefi_call_wrapper(
        BS->LocateHandleBuffer, 5, ByProtocol,
        &FileSystemProtocol, NULL, &HandleCount, &Handles
    );

    if (EFI_ERROR(Status)) {
        Print(L"Error locating file system: %r\n\r", Status);
        return Status;
    }

    Print(L"Found %llu handles supporting EFI_SIMPLE_FILE_SYSTEM_PROTOCOL\n\r", HandleCount);

    // Iterate through all the available handles to find the one containing kernel.elf
    for (UINTN i = 0; i < HandleCount; i++) {
        // Obtain the file system protocol for the current handle
        Status = uefi_call_wrapper(BS->HandleProtocol, 3, Handles[i], &FileSystemProtocol, (VOID**)&FileSystem);

        if (EFI_ERROR(Status)) {
            Print(L"Error obtaining file system on handle %llu: %r\n\r", i, Status);
            continue; // Try the next handle
        }

        // Open the root volume
        Status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &TempRootDir);
        if (EFI_ERROR(Status)) {
            Print(L"Error opening root volume on handle %llu: %r\n\r", i, Status);
            continue; // Try the next handle
        }

        // Attempt to open "kernel.elf" in this root directory
        Status = uefi_call_wrapper(
            TempRootDir->Open,
            5,
            TempRootDir,
            &TempFile,
            L"kernel.elf",
            EFI_FILE_MODE_READ,
            0
        );

        if (!EFI_ERROR(Status)) {
            Print(L"kernel.elf found on handle %llu\n\r", i);
            *RootDir = TempRootDir; // Return the correct root directory
            return EFI_SUCCESS;
        } else {
            Print(L"kernel.elf not found on handle %llu\n\r", i);
        }

        // Close the temporary root directory as it's not the correct one
        TempRootDir->Close(TempRootDir);
    }

    Print(L"Failed to find kernel.elf on any handle\n\r");
    return EFI_NOT_FOUND;  // Return error if no root directory with kernel.elf is found
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
