#ifndef FILE_LOADER_H
#define FILE_LOADER_H
#include "common.h"

EFI_STATUS OpenRootDirectory(
    EFI_FILE** RootDir
);

EFI_STATUS OpenFile(
    EFI_FILE* RootDir,
    CHAR16* FileName,
    EFI_FILE** File
);

#endif
