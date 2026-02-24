#include "debug/symtab.h"
#include "debug/elf64.h"
#include "common/string.h"
#include "common/logging.h"
#include "mm/heap.h"

namespace symtab {

static elf64::Sym* g_symtab      = nullptr;
static const char* g_strtab      = nullptr;
static uint64_t    g_sym_count   = 0;
static uint64_t    g_strtab_size = 0;
static bool        g_available   = false;

__PRIVILEGED_CODE int32_t init(const debug::kernel_elf& elf) {
    const elf64::Shdr* symtab_shdr = nullptr;
    for (uint16_t i = 0; i < elf.shnum; i++) {
        if (elf.shdr_base[i].sh_type == elf64::SHT_SYMTAB) {
            symtab_shdr = &elf.shdr_base[i];
            break;
        }
    }

    if (!symtab_shdr) {
        log::warn("symtab: no .symtab section found");
        return ERR_NO_SYMTAB;
    }

    if (symtab_shdr->sh_link == 0 || symtab_shdr->sh_link >= elf.shnum) {
        log::warn("symtab: .symtab has invalid strtab link");
        return ERR_BAD_ELF;
    }

    const elf64::Shdr* strtab_shdr = &elf.shdr_base[symtab_shdr->sh_link];
    if (strtab_shdr->sh_type != elf64::SHT_STRTAB) {
        log::warn("symtab: linked section is not a string table");
        return ERR_BAD_ELF;
    }

    uint64_t symtab_size = symtab_shdr->sh_size;
    uint64_t strtab_size = strtab_shdr->sh_size;
    uint64_t sym_count = symtab_size / sizeof(elf64::Sym);

    if (symtab_shdr->sh_offset + symtab_size > elf.file_size ||
        strtab_shdr->sh_offset + strtab_size > elf.file_size) {
        log::warn("symtab: section data extends beyond kernel file");
        return ERR_BAD_ELF;
    }

    auto* sym_copy = reinterpret_cast<elf64::Sym*>(heap::kalloc(symtab_size));
    if (!sym_copy) {
        log::warn("symtab: failed to allocate for symbol table");
        return ERR_NO_MEMORY;
    }

    auto* str_copy = reinterpret_cast<char*>(heap::kalloc(strtab_size));
    if (!str_copy) {
        heap::kfree(sym_copy);
        log::warn("symtab: failed to allocate for string table");
        return ERR_NO_MEMORY;
    }

    string::memcpy(sym_copy, elf.base + symtab_shdr->sh_offset, symtab_size);
    string::memcpy(str_copy, elf.base + strtab_shdr->sh_offset, strtab_size);

    g_symtab      = sym_copy;
    g_strtab      = str_copy;
    g_sym_count   = sym_count;
    g_strtab_size = strtab_size;
    g_available   = true;

    log::info("symtab: loaded %lu symbols", sym_count);
    return OK;
}

bool resolve(uint64_t addr, resolve_result* out) {
    if (!g_available || !out) return false;
    const elf64::Sym* best = nullptr;
    for (uint64_t i = 0; i < g_sym_count; i++) {
        const elf64::Sym* sym = &g_symtab[i];
        if (elf64::sym_type(sym) != elf64::STT_FUNC) continue;
        if (sym->st_shndx == elf64::SHN_UNDEF) continue;
        if (sym->st_value == 0) continue;
        if (sym->st_value > addr) continue;
        if (sym->st_size > 0 && addr >= sym->st_value + sym->st_size) continue;
        if (!best || sym->st_value > best->st_value) best = sym;
    }
    if (!best) return false;
    if (best->st_name >= g_strtab_size) return false;
    out->name   = g_strtab + best->st_name;
    out->offset = addr - best->st_value;
    return true;
}

} // namespace symtab
