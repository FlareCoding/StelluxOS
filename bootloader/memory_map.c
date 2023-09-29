#include "memory_map.h"

UINT64 GetTotalSystemMemory(
    EFI_MEMORY_DESCRIPTOR* MemoryMap,
    UINT64 Entries,
    UINT64 DescriptorSize
) {
    UINT64 TotalMemory = 0;

    for (UINT64 i = 0; i < Entries; ++i) {
        EFI_MEMORY_DESCRIPTOR* desc =
            (EFI_MEMORY_DESCRIPTOR*)((UINT64)MemoryMap + (i * DescriptorSize));

        TotalMemory += desc->NumberOfPages * PAGE_SIZE;
    }

    return TotalMemory;
}

EFI_MEMORY_DESCRIPTOR* ReadMemoryMap(
    UINTN* MemoryMapSize,
    UINTN* MemoryMapKey,
    UINTN* DescriptorSize,
    UINT64* TotalSystemMemory
) {
    EFI_STATUS                  Status;
    EFI_MEMORY_DESCRIPTOR       *EfiMemoryMap;
    UINT32                      EfiDescriptorVersion;

    //
    // Get the EFI memory map.
    //
    EfiMemoryMap      = NULL;
    Status = gBS->GetMemoryMap (
        MemoryMapSize,
        EfiMemoryMap,
        MemoryMapKey,
        DescriptorSize,
        &EfiDescriptorVersion
    );
    ASSERT(Status == EFI_BUFFER_TOO_SMALL);

    //
    // Use size returned for the AllocatePool.
    //
    EfiMemoryMap = (EFI_MEMORY_DESCRIPTOR*)AllocatePool(*MemoryMapSize + 2 * *DescriptorSize);
    ASSERT(EfiMemoryMap != NULL);
    Status = gBS->GetMemoryMap (
        MemoryMapSize,
        EfiMemoryMap,
        MemoryMapKey,
        DescriptorSize,
        &EfiDescriptorVersion
    );
    
    if (EFI_ERROR(Status)) {
        FreePool(EfiMemoryMap);
        return NULL;
    }

    UINT64 DescriptorCount = *MemoryMapSize / *DescriptorSize;
    *TotalSystemMemory = GetTotalSystemMemory(EfiMemoryMap, DescriptorCount, *DescriptorSize);

    return EfiMemoryMap;
}
