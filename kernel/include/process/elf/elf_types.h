#ifndef ELF_TYPES_H
#define ELF_TYPES_H
#include <types.h>

namespace elf {
// ELF Identification Indexes
constexpr uint8_t EI_MAG0 = 0;
constexpr uint8_t EI_MAG1 = 1;
constexpr uint8_t EI_MAG2 = 2;
constexpr uint8_t EI_MAG3 = 3;
constexpr uint8_t EI_CLASS = 4;
constexpr uint8_t EI_DATA = 5;
constexpr uint8_t EI_VERSION = 6;

// ELF Magic Numbers
constexpr uint8_t ELF_MAGIC0 = 0x7F;
constexpr uint8_t ELF_MAGIC1 = 'E';
constexpr uint8_t ELF_MAGIC2 = 'L';
constexpr uint8_t ELF_MAGIC3 = 'F';

// ELF Class
constexpr uint8_t ELFCLASS64 = 2;

// ELF Data Encoding
constexpr uint8_t ELFDATA2LSB = 1;

// ELF Header Type
constexpr uint16_t ET_EXEC = 2; // Executable file

// ELF Machine Type
constexpr uint16_t EM_X86_64 = 62; // AMD x86-64

// ELF Program Header Types
constexpr uint32_t PT_NULL = 0x00000000;
constexpr uint32_t PT_LOAD = 0x00000001;
constexpr uint32_t PT_DYNAMIC = 0x00000002;
constexpr uint32_t PT_INTERP = 0x00000003;
constexpr uint32_t PT_NOTE = 0x00000004;
constexpr uint32_t PT_SHLIB = 0x00000005;
constexpr uint32_t PT_PHDR = 0x00000006;
constexpr uint32_t PT_GNU_STACK = 0x6474e551;

// ELF Program Header Flags
constexpr uint32_t PF_X = 0x1; // Execute
constexpr uint32_t PF_W = 0x2; // Write
constexpr uint32_t PF_R = 0x4; // Read

// ELF Header (ELF64)
struct elf64_ehdr {
    uint8_t e_ident[16];     // ELF Identification bytes
    uint16_t e_type;         // Object file type
    uint16_t e_machine;      // Machine type
    uint32_t e_version;      // Object file version
    uint64_t e_entry;        // Entry point address
    uint64_t e_phoff;        // Program header offset
    uint64_t e_shoff;        // Section header offset
    uint32_t e_flags;        // Processor-specific flags
    uint16_t e_ehsize;       // ELF header size
    uint16_t e_phentsize;    // Size of a program header entry
    uint16_t e_phnum;        // Number of program header entries
    uint16_t e_shentsize;    // Size of a section header entry
    uint16_t e_shnum;        // Number of section header entries
    uint16_t e_shstrndx;     // Section name string table index
};

// Program Header (ELF64)
struct elf64_phdr {
    uint32_t p_type;         // Type of segment
    uint32_t p_flags;        // Segment attributes
    uint64_t p_offset;       // Offset in file
    uint64_t p_vaddr;        // Virtual address in memory
    uint64_t p_paddr;        // Physical address (unused on many platforms)
    uint64_t p_filesz;       // Size of segment in file
    uint64_t p_memsz;        // Size of segment in memory
    uint64_t p_align;        // Alignment of segment
};

// String Header (ELF64)
struct elf64_shdr {
    uint32_t sh_name;        // Section name (string table index)
    uint32_t sh_type;        // Section type
    uint64_t sh_flags;       // Section attributes
    uint64_t sh_addr;        // Virtual address in memory
    uint64_t sh_offset;      // Offset in file
    uint64_t sh_size;        // Size of section
    uint32_t sh_link;        // Link to other section
    uint32_t sh_info;        // Miscellaneous information
    uint64_t sh_addralign;   // Address alignment boundary
    uint64_t sh_entsize;     // Size of entries, if section has a table
};
} // namespace elf

#endif // ELF_TYPES_H
