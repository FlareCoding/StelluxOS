#include "elf_loader.h"

EFI_STATUS OpenRootDirectory(
    EFI_FILE** RootDir
) {
    EFI_STATUS Status;
    UINTN HandleCount;
    EFI_HANDLE* Handles;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem;

    // Locate handle(s) that support EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    Status = uefi_call_wrapper(
        BS->LocateHandleBuffer, 5, ByProtocol,
        &FileSystemProtocol, NULL, &HandleCount, &Handles
    );

    if (EFI_ERROR(Status)) {
        Print(L"Error locating file system: %r\n\r", Status);
        return Status;
    }

    // Usually, the file system is on the first handle, additional error checking is advised for production
    Status = uefi_call_wrapper(
        BS->HandleProtocol, 3, Handles[0],
        &FileSystemProtocol, (VOID**)&FileSystem
    );

    if (EFI_ERROR(Status)) {
        Print(L"Error obtaining file system: %r\n\r", Status);
        return Status;
    }

    // Open the root volume
    Status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, RootDir);
    if (EFI_ERROR(Status)) {
        Print(L"Error opening root volume: %r\n\r", Status);
        return Status;
    }

    return EFI_SUCCESS;
}

EFI_STATUS OpenFile(EFI_FILE* RootDir, CHAR16* FileName, EFI_FILE** File) {
    EFI_STATUS Status;
    Status = uefi_call_wrapper(
        RootDir->Open,
        5,
        RootDir,
        File,
        FileName,
        EFI_FILE_MODE_READ,
        0
    );
    
    if (EFI_ERROR(Status)) {
        Print(L"Error opening file: %r\n\r", Status);
        return Status;
    }

    return EFI_SUCCESS;
}

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

EFI_STATUS LoadElfSegments(
    EFI_FILE* File,
    Elf64_Phdr* ProgramHeaders,
    UINT16 NumHeaders,
    VOID** PhysicalBase,
    VOID** VirtualBase,
    UINT64* TotalSize
) {
    EFI_STATUS Status;
    *TotalSize = 0;

    for (UINT16 i = 0; i < NumHeaders; ++i) {
        Elf64_Phdr* phdr = &ProgramHeaders[i];

        if (phdr->p_type != PT_LOAD) {
            continue;  // Skip non-loadable segments
        }

        // Attempt to allocate memory at the desired physical address first
        VOID* Segment = (VOID*)(UINTN)phdr->p_paddr;
        UINTN Pages = EFI_SIZE_TO_PAGES(phdr->p_memsz);

        Status = uefi_call_wrapper(
            BS->AllocatePages,
            4,
            AllocateAddress,
            EfiLoaderData,
            Pages,
            (EFI_PHYSICAL_ADDRESS*)&Segment
        );

        // If that fails, try allocating anywhere
        if (EFI_ERROR(Status)) {
            Segment = NULL; // Reset Segment pointer
            Status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, Pages, (EFI_PHYSICAL_ADDRESS*)&Segment);
        }

        if (EFI_ERROR(Status)) {
            Print(L"Error allocating pages: %r\n\r", Status);
            return Status;
        }

        // Read the segment from the file into the allocated memory
        Status = uefi_call_wrapper(File->SetPosition, 2, File, phdr->p_offset);
        UINTN Size = phdr->p_filesz;

        Status = uefi_call_wrapper(File->Read, 3, File, &Size, Segment);
        if (EFI_ERROR(Status)) {
            Print(L"Error reading segment: %r\n\r", Status);
            return Status;
        }

        *TotalSize += phdr->p_memsz;

        // Zero out the remaining memory (if any)
        if (phdr->p_memsz > phdr->p_filesz) {
            SetMem(Segment + phdr->p_filesz, phdr->p_memsz - phdr->p_filesz, 0);
        }

        if (i == 0) {  // Assuming the first segment is the lowest
            *PhysicalBase = Segment;
            *VirtualBase = (void*)phdr->p_vaddr;
        }
    }

    return EFI_SUCCESS;
}


EFI_STATUS LoadElfFile(
    EFI_FILE* RootDir,
    CHAR16* FileName,
    VOID** EntryPoint,
    VOID** ElfPhysicalBase,
    VOID** ElfVirtualBase,
    UINT64* ElfSize
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

    Status = LoadElfSegments(
        ElfFile,
        ProgramHeaders,
        ElfHeader.e_phnum,
        ElfPhysicalBase,
        ElfVirtualBase,
        ElfSize
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
