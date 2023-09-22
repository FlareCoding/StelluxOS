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
    EFI_MEMORY_DESCRIPTOR** EfiMemoryMap,
    UINTN* MemoryMapSize,
    UINTN* MemoryMapKey,
    UINTN* DescriptorSize,
    UINT64* TotalSystemMemory
) {
    EFI_STATUS Status;
    UINT32 DescriptorVersion;

    // First call will just give us the map size
    uefi_call_wrapper(
        gBS->GetMemoryMap,
        5,
        MemoryMapSize,
        *EfiMemoryMap,
        MemoryMapKey,
        DescriptorSize,
        &DescriptorVersion
    );

    // Allocate enough space for the memory map
    *EfiMemoryMap = AllocateZeroPool(*MemoryMapSize);

    // Actually read in the memory map
    Status = uefi_call_wrapper(
        gBS->GetMemoryMap,
        5,
        MemoryMapSize,
        *EfiMemoryMap,
        MemoryMapKey,
        DescriptorSize,
        &DescriptorVersion
    );

    if (EFI_ERROR(Status)) {
        Print(L"Failed to read memory map: %r\n\r", Status);
        return Status;
    }

    UINT64 DescriptorCount = *MemoryMapSize / *DescriptorSize;
    *TotalSystemMemory = GetTotalSystemMemory(*EfiMemoryMap, DescriptorCount, *DescriptorSize);

    return EFI_SUCCESS;
}
