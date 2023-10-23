#include "elf_loader.h"

EFI_STATUS ValidateElfHeader(Elf64_Ehdr* ElfHeader) {
    // Check the magic numbers
    if (ElfHeader->e_ident[EI_MAG0] != ELFMAG0 ||
        ElfHeader->e_ident[EI_MAG1] != ELFMAG1 ||
        ElfHeader->e_ident[EI_MAG2] != ELFMAG2 ||
        ElfHeader->e_ident[EI_MAG3] != ELFMAG3) {
        Print(L"Invalid ELF magic numbers.\n\r");
        return EFI_UNSUPPORTED;
    }
    
    // Check the architecture (we support x86-64 in this case)
    if (ElfHeader->e_ident[EI_CLASS] != ELFCLASS64 ||
        ElfHeader->e_machine != EM_X86_64) {
        Print(L"Unsupported architecture.\n\r");
        return EFI_UNSUPPORTED;
    }

    // Check the ELF file type (must be an executable)
    if (ElfHeader->e_type != ET_EXEC) {
        Print(L"Unsupported ELF file type.\n\r");
        return EFI_UNSUPPORTED;
    }

    return EFI_SUCCESS;
}

EFI_STATUS ReadElfHeader(EFI_FILE* File, Elf64_Ehdr* ElfHeader) {
    EFI_STATUS Status;
    UINTN HeaderSize = sizeof(Elf64_Ehdr);
    
    // Reading the ELF header
    Status = uefi_call_wrapper(File->Read, 3, File, &HeaderSize, ElfHeader);
    if (EFI_ERROR(Status)) {
        Print(L"Error reading ELF header: %r\n\r", Status);
        return Status;
    }

    // Validate if what we read is sufficient
    if (HeaderSize != sizeof(Elf64_Ehdr)) {
        Print(L"Read incorrect amount of bytes for ELF header. Expected %d bytes but read %d bytes.\n\r", sizeof(Elf64_Ehdr), HeaderSize);
        return EFI_LOAD_ERROR;
    }

    Status = ValidateElfHeader(ElfHeader);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to validate ELF header: %r\n\r", Status);
        return Status;
    }
    
    return EFI_SUCCESS;
}

EFI_STATUS ReadProgramHeaders(
    EFI_FILE* File,
    Elf64_Ehdr* ElfHeader,
    Elf64_Phdr** ProgramHeaders
) {
    EFI_STATUS Status;

    // Move the file pointer to the beginning of the program header table
    Status = uefi_call_wrapper(File->SetPosition, 2, File, ElfHeader->e_phoff);
    if (EFI_ERROR(Status)) {
        Print(L"Error setting file position to program headers: %r\n\r", Status);
        return Status;
    }

    // Allocate space for the program headers
    *ProgramHeaders = AllocatePool(ElfHeader->e_phentsize * ElfHeader->e_phnum);
    if (*ProgramHeaders == NULL) {
        Print(L"Error allocating memory for program headers.\n\r");
        return EFI_OUT_OF_RESOURCES;
    }

    // Read the program headers into the allocated memory
    UINTN Size = ElfHeader->e_phentsize * ElfHeader->e_phnum;
    Status = uefi_call_wrapper(File->Read, 3, File, &Size, (VOID*)*ProgramHeaders);

    if (EFI_ERROR(Status) || Size != (ElfHeader->e_phentsize * ElfHeader->e_phnum)) {
        Print(L"Error reading program headers: %r\n\r", Status);
        FreePool(*ProgramHeaders);
        return Status == EFI_SUCCESS ? EFI_LOAD_ERROR : Status;
    }

    return EFI_SUCCESS;
}

EFI_STATUS LoadElfSections(
    EFI_FILE* File,
    Elf64_Ehdr* elfHeader,
    struct ElfSectionInfo* SectionInfoList,
    UINT64* SectionCount
) {
    EFI_STATUS Status;
    UINT64 offset = elfHeader->e_shoff;
    UINT16 numSections = elfHeader->e_shnum;
    UINT16 stringTableIndex = elfHeader->e_shstrndx;
    Elf64_Shdr* sectionHeaders = NULL;
    CHAR8* stringTable = NULL;

    // Allocate memory for section headers
    Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, numSections * sizeof(Elf64_Shdr), (VOID**)&sectionHeaders);
    if (EFI_ERROR(Status)) {
        Print(L"Error allocating memory for section headers: %r\n\r", Status);
        return Status;
    }

    // Read section headers
    Status = uefi_call_wrapper(File->SetPosition, 2, File, offset);
    UINTN size = numSections * sizeof(Elf64_Shdr);
    Status = uefi_call_wrapper(File->Read, 3, File, &size, sectionHeaders);
    if (EFI_ERROR(Status)) {
        Print(L"Error reading section headers: %r\n\r", Status);
        return Status;
    }

    // Read string table section header
    Elf64_Shdr* stringTableHeader = &sectionHeaders[stringTableIndex];
    
    // Allocate and read string table
    Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, stringTableHeader->sh_size, (VOID**)&stringTable);
    Status = uefi_call_wrapper(File->SetPosition, 2, File, stringTableHeader->sh_offset);
    size = stringTableHeader->sh_size;
    Status = uefi_call_wrapper(File->Read, 3, File, &size, stringTable);
    if (EFI_ERROR(Status)) {
        Print(L"Error reading string table: %r\n\r", Status);
        return Status;
    }

    // Now, we can start populating the SectionInfoList
    *SectionCount = (UINT64)numSections;

    for (UINT16 i = 0; i < numSections; ++i) {
        Elf64_Shdr* shdr = &sectionHeaders[i];
        CHAR8* sectionName = stringTable + shdr->sh_name;

        SectionInfoList[i].Name = sectionName;
        SectionInfoList[i].VirtualBase = shdr->sh_addr;
        SectionInfoList[i].VirtualSize = shdr->sh_size;
        SectionInfoList[i].Flags = shdr->sh_flags;

        if (
            strncmp(sectionName, ".ktext", 6) == 0 ||
            strncmp(sectionName, ".kdata", 6) == 0 ||
            strncmp(sectionName, ".krodata", 8) == 0
        ) {
            SectionInfoList[i].Privileged = 1;
        } else {
            SectionInfoList[i].Privileged = 0;
        }
    }

    return EFI_SUCCESS;
}

EFI_STATUS LoadElfSegments(
    EFI_FILE* File,
    Elf64_Phdr* ProgramHeaders,
    UINT16 NumHeaders,
    VOID** PhysicalBase,
    VOID** VirtualBase,
    UINT64* TotalSize,
    struct ElfSegmentInfo* ElfSegmentList,
    UINT64* ElfSegmentCount
) {
    EFI_STATUS Status;
    *TotalSize = 0;

    // Calculate total size required for a contiguous block
    for (UINT16 i = 0; i < NumHeaders; ++i) {
        Elf64_Phdr* phdr = &ProgramHeaders[i];
        if (phdr->p_type == PT_LOAD) {
            *TotalSize += phdr->p_memsz;
        }
    }

    // Allocate contiguous memory block
    VOID* ContiguousBase = NULL;
    UINTN TotalPages = EFI_SIZE_TO_PAGES(*TotalSize);
    Status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, TotalPages, (EFI_PHYSICAL_ADDRESS*)&ContiguousBase);
    if (EFI_ERROR(Status)) {
        Print(L"Error allocating contiguous pages: %r\n\r", Status);
        return Status;
    }

    *PhysicalBase = ContiguousBase;

    UINT64 Offset = 0;
    UINT64 SegmentIndex = 0;

    // Load each segment
    for (UINT16 i = 0; i < NumHeaders; ++i) {
        Elf64_Phdr* phdr = &ProgramHeaders[i];

        if (phdr->p_type != PT_LOAD) {
            continue;  // Skip non-loadable segments
        }

        // Calculate destination pointer within the contiguous block
        VOID* Segment = ContiguousBase + Offset;

        // Read the segment from the file into the allocated memory
        Status = uefi_call_wrapper(File->SetPosition, 2, File, phdr->p_offset);
        UINTN Size = phdr->p_filesz;

        Status = uefi_call_wrapper(File->Read, 3, File, &Size, Segment);
        if (EFI_ERROR(Status)) {
            Print(L"Error reading segment: %r\n\r", Status);
            return Status;
        }

        // Zero out the remaining memory (if any)
        if (phdr->p_memsz > phdr->p_filesz) {
            SetMem(Segment + phdr->p_filesz, phdr->p_memsz - phdr->p_filesz, 0);
        }

        // Assuming the first segment is the lowest
        if (i == 0) {
            *VirtualBase = (VOID*)phdr->p_vaddr;
        }

        struct ElfSegmentInfo SegInfo;
        SegInfo.PhysicalBase = Segment;
        SegInfo.PhysicalSize = (UINT64)phdr->p_memsz;
        SegInfo.VirtualBase = (VOID*)phdr->p_vaddr;
        SegInfo.VirtualSize = (UINT64)phdr->p_memsz;
        SegInfo.Flags = 0;

        ElfSegmentList[SegmentIndex++] = SegInfo;

        (*ElfSegmentCount)++;

        // Update the offset for the next segment
        Offset += phdr->p_memsz;
    }

    return EFI_SUCCESS;
}

EFI_STATUS LoadElfFile(
    EFI_FILE* RootDir,
    CHAR16* FileName,
    VOID** EntryPoint,
    VOID** ElfPhysicalBase,
    VOID** ElfVirtualBase,
    UINT64* ElfSize,
    struct ElfSegmentInfo* ElfSegmentList,
    UINT64* ElfSegmentCount,
    struct ElfSectionInfo* ElfSectionList,
    UINT64* ElfSectionCount
) {
    EFI_STATUS Status;
    EFI_FILE* ElfFile;
    Elf64_Ehdr ElfHeader;
    Elf64_Phdr* ProgramHeaders;

    Status = OpenFile(RootDir, FileName, &ElfFile);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to open kernel file.\n\r");
        return Status;
    }

    Status = ReadElfHeader(ElfFile, &ElfHeader);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to read or validate ELF header.\n\r");
        uefi_call_wrapper(ElfFile->Close, 1, ElfFile);
        return Status;
    }
    
    Status = ReadProgramHeaders(ElfFile, &ElfHeader, &ProgramHeaders);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to read ELF program headers.\n\r");
        uefi_call_wrapper(ElfFile->Close, 1, ElfFile);
        return Status;
    }

    Status = LoadElfSections(ElfFile, &ElfHeader, ElfSectionList, ElfSectionCount);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to read ELF program headers.\n\r");
        uefi_call_wrapper(ElfFile->Close, 1, ElfFile);
        return Status;
    }

    Status = LoadElfSegments(
        ElfFile,
        ProgramHeaders,
        ElfHeader.e_phnum,
        ElfPhysicalBase,
        ElfVirtualBase,
        ElfSize,
        ElfSegmentList,
        ElfSegmentCount
    );
    if (EFI_ERROR(Status)) {
        Print(L"Failed to load ELF segments.\n\r");
        FreePool(ProgramHeaders);
        uefi_call_wrapper(ElfFile->Close, 1, ElfFile);
        return Status;
    }

    *EntryPoint = (VOID*)ElfHeader.e_entry;

    FreePool(ProgramHeaders);
    uefi_call_wrapper(ElfFile->Close, 1, ElfFile);
    return EFI_SUCCESS;
}
