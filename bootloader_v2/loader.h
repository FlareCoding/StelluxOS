#ifndef LOADER_H
#define LOADER_H

#define UNICODE
#include <efi.h>
#include <efilib.h>
#include <elf.h>

EFI_FILE*
LoadFile(
    EFI_FILE* Dir,
    CHAR16* Path,
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE* SystemTable
);

EFI_FILE_INFO*
GetFileInfo(
    EFI_FILE* File
);

EFI_STATUS
ReadKernelHeader(
    IN EFI_FILE* KernelEfiFile,
    OUT Elf64_Ehdr* Hdr
);

EFI_STATUS
VerifyKernelHeader(
    Elf64_Ehdr* Hdr
);

#endif
