#ifndef STELLUX_EXEC_ELF64_H
#define STELLUX_EXEC_ELF64_H

#include "common/types.h"

namespace elf64 {

constexpr uint8_t ELFMAG0 = 0x7F;
constexpr uint8_t ELFMAG1 = 'E';
constexpr uint8_t ELFMAG2 = 'L';
constexpr uint8_t ELFMAG3 = 'F';
constexpr uint8_t ELFCLASS64 = 2;

// e_ident indices
constexpr uint8_t EI_CLASS   = 4;
constexpr uint8_t EI_DATA    = 5;
constexpr uint8_t EI_VERSION = 6;

// e_ident values
constexpr uint8_t ELFDATA2LSB = 1;
constexpr uint8_t EV_CURRENT  = 1;

// e_type
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t ET_DYN  = 3;

// e_machine
constexpr uint16_t EM_X86_64  = 0x3E;
constexpr uint16_t EM_AARCH64 = 0xB7;

// p_type
constexpr uint32_t PT_NULL = 0;
constexpr uint32_t PT_LOAD = 1;

// p_flags
constexpr uint32_t PF_X = 0x1;
constexpr uint32_t PF_W = 0x2;
constexpr uint32_t PF_R = 0x4;

// Section header types
constexpr uint32_t SHT_NULL    = 0;
constexpr uint32_t SHT_SYMTAB = 2;
constexpr uint32_t SHT_STRTAB = 3;

// Symbol binding
constexpr uint8_t STB_LOCAL  = 0;
constexpr uint8_t STB_GLOBAL = 1;
constexpr uint8_t STB_WEAK   = 2;

// Symbol type
constexpr uint8_t STT_NOTYPE = 0;
constexpr uint8_t STT_OBJECT = 1;
constexpr uint8_t STT_FUNC   = 2;

constexpr uint16_t SHN_UNDEF = 0;

struct Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct Sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

inline uint8_t sym_type(const Sym* s) {
    return s->st_info & 0x0F;
}

inline uint8_t sym_bind(const Sym* s) {
    return s->st_info >> 4;
}

static_assert(sizeof(Ehdr) == 64, "Ehdr must be 64 bytes");
static_assert(sizeof(Phdr) == 56, "Phdr must be 56 bytes");
static_assert(sizeof(Shdr) == 64, "Shdr must be 64 bytes");
static_assert(sizeof(Sym)  == 24, "Sym must be 24 bytes");

} // namespace elf64

#endif // STELLUX_EXEC_ELF64_H
