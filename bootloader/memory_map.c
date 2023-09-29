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

EFI_STATUS ReadMemoryMap(
    EFI_MEMORY_DESCRIPTOR** xEfiMemoryMap,
    UINTN* xMemoryMapSize,
    UINTN* xMemoryMapKey,
    UINTN* xDescriptorSize,
    UINT64* xTotalSystemMemory
) {
    EFI_STATUS                  Status;
    EFI_MEMORY_DESCRIPTOR       *EfiMemoryMap;
    UINTN                       EfiMemoryMapSize;
    UINTN                       EfiMapKey;
    UINTN                       EfiDescriptorSize;
    UINT32                      EfiDescriptorVersion;

    //
    // Get the EFI memory map.
    //
    EfiMemoryMapSize  = 0;
    EfiMemoryMap      = NULL;
    Status = gBS->GetMemoryMap (
        &EfiMemoryMapSize,
        EfiMemoryMap,
        &EfiMapKey,
        &EfiDescriptorSize,
        &EfiDescriptorVersion
    );
    ASSERT (Status == EFI_BUFFER_TOO_SMALL);

    //
    // Use size returned for the AllocatePool.
    //
    EfiMemoryMap = (EFI_MEMORY_DESCRIPTOR *) AllocatePool (EfiMemoryMapSize + 2 * EfiDescriptorSize);
    ASSERT (EfiMemoryMap != NULL);
    Status = gBS->GetMemoryMap (
        &EfiMemoryMapSize,
        EfiMemoryMap,
        &EfiMapKey,
        &EfiDescriptorSize,
        &EfiDescriptorVersion
    );
    
    if (EFI_ERROR (Status)) {
        FreePool (EfiMemoryMap);
    }

    Print(L"Acquired Memory Map!\n");

    UINT64 DescriptorCount = EfiMemoryMapSize / EfiDescriptorSize;
    UINT64 TotalSystemMemory = GetTotalSystemMemory(EfiMemoryMap, DescriptorCount, EfiDescriptorSize);

    Print(L"Total system memory: 0x%llx (%llu GB)\n", TotalSystemMemory, TotalSystemMemory / 1024 / 1024 / 1024);

    return EFI_SUCCESS;
}
