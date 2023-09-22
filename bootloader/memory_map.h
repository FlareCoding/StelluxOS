#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H
#include "common.h"

EFI_STATUS ReadMemoryMap(
    EFI_MEMORY_DESCRIPTOR** EfiMemoryMap,
    UINTN* MemoryMapSize,
    UINTN* MemoryMapKey,
    UINTN* DescriptorSize,
    UINT64* TotalSystemMemory
);

#endif // MEMORY_MAP_H