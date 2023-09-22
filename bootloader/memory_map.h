#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H
#include "common.h"

typedef struct {
    UINT64 BaseAddress;
    UINT64 Size;
    UINT32 Type;
} StelluxMemoryDescriptor;

EFI_STATUS RetrieveMemoryMap(StelluxMemoryDescriptor** CustomMap, UINTN* MapSize, UINTN* MapKey);

#endif // MEMORY_MAP_H