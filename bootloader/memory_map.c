#include "memory_map.h"

#define PAGE_SIZE 4096

EFI_STATUS RetrieveMemoryMap(StelluxMemoryDescriptor** CustomMap, UINTN* MapSize, UINTN* MapKey) {
    EFI_STATUS Status;
    EFI_MEMORY_DESCRIPTOR* UefiMemoryMap = NULL;
    UINTN MemoryMapSize = 0;
    UINTN MKey;
    UINTN DescriptorSize;
    UINT32 DescriptorVersion;
    UINTN DescriptorCount;

    // Get Memory Map size first
    Status = uefi_call_wrapper(BS->GetMemoryMap, 5, &MemoryMapSize, UefiMemoryMap, &MKey, &DescriptorSize, &DescriptorVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"Error fetching memory map size: %r\n", Status);
        return Status;
    }

    // Add some extra size for potential inaccuracy
    MemoryMapSize += 8 * DescriptorSize;

    // Allocate pool for UEFI's memory map
    Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, MemoryMapSize, (void**)&UefiMemoryMap);
    if (EFI_ERROR(Status)) {
        Print(L"Error allocating memory for UEFI memory map: %r\n", Status);
        return Status;
    }

    // Get the actual memory map
    Status = uefi_call_wrapper(BS->GetMemoryMap, 5, &MemoryMapSize, UefiMemoryMap, &MKey, &DescriptorSize, &DescriptorVersion);
    if (EFI_ERROR(Status)) {
        Print(L"Error fetching memory map: %r\n", Status);
        return Status;
    }

    // Calculate the number of descriptors
    DescriptorCount = MemoryMapSize / DescriptorSize;

    // Allocate memory for custom map
    Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, DescriptorCount * sizeof(StelluxMemoryDescriptor), (void**)CustomMap);
    if (EFI_ERROR(Status)) {
        Print(L"Error allocating memory for custom memory map: %r\n", Status);
        return Status;
    }

    // Fill in custom map
    for (UINTN i = 0; i < DescriptorCount; ++i) {
        EFI_MEMORY_DESCRIPTOR* Descriptor = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)UefiMemoryMap + (i * DescriptorSize));
        StelluxMemoryDescriptor* CustomDescriptor = &(*CustomMap)[i];

        CustomDescriptor->BaseAddress = Descriptor->PhysicalStart;
        CustomDescriptor->Size = Descriptor->NumberOfPages * 4096;  // Assuming 4KB page size
        CustomDescriptor->Type = Descriptor->Type;
    }

    // Save the size for later use
    *MapSize = DescriptorCount;
    *MapKey = MKey;

    // Free UEFI memory map as we no longer need it
    uefi_call_wrapper(BS->FreePool, 1, UefiMemoryMap);

    return EFI_SUCCESS;
}
