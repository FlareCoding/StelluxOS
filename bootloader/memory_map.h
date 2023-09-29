#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H
#include "common.h"

EFI_MEMORY_DESCRIPTOR* ReadMemoryMap(
    UINTN* MemoryMapSize,
    UINTN* MemoryMapKey,
    UINTN* DescriptorSize,
    UINT64* TotalSystemMemory
);

#endif // MEMORY_MAP_H