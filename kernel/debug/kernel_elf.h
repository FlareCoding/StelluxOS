#ifndef STELLUX_DEBUG_KERNEL_ELF_H
#define STELLUX_DEBUG_KERNEL_ELF_H

#include "exec/elf64.h"
#include "common/string.h"

namespace debug {

struct kernel_elf {
    const uint8_t*     base;
    const elf64::Ehdr* ehdr;
    const elf64::Shdr* shdr_base;
    const char*        shstrtab;
    uint64_t           shstrtab_size;
    uint64_t           file_size;
    uint16_t           shnum;

    const elf64::Shdr* find_section(const char* name) const {
        for (uint16_t i = 0; i < shnum; i++) {
            uint32_t name_off = shdr_base[i].sh_name;
            if (name_off >= shstrtab_size) continue;
            if (string::strcmp(shstrtab + name_off, name) == 0) {
                return &shdr_base[i];
            }
        }
        return nullptr;
    }
};

} // namespace debug

#endif // STELLUX_DEBUG_KERNEL_ELF_H
