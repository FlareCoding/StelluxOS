#include "debug/symtab.h"
#include "debug/elf64.h"
#include "boot/boot_services.h"
#include "common/string.h"
#include "common/logging.h"
#include "mm/heap.h"
#include "mm/paging.h"

namespace symtab {

static elf64::Sym* g_symtab      = nullptr;
static const char* g_strtab      = nullptr;
static uint64_t    g_sym_count   = 0;
static uint64_t    g_strtab_size = 0;
static bool        g_available   = false;

__PRIVILEGED_CODE static void ensure_mapped(uintptr_t virt, uint64_t size) {
    uintptr_t hhdm = g_boot_info.hhdm_offset;
    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    uintptr_t page_start = virt & ~(paging::PAGE_SIZE_4KB - 1);
    uintptr_t page_end   = (virt + size + paging::PAGE_SIZE_4KB - 1) & ~(paging::PAGE_SIZE_4KB - 1);

    for (uintptr_t v = page_start; v < page_end; v += paging::PAGE_SIZE_4KB) {
        if (paging::is_mapped(v, root)) continue;
        pmm::phys_addr_t phys = v - hhdm;
        paging::map_page(v, phys, paging::PAGE_KERNEL_RO, root);
    }
}

__PRIVILEGED_CODE int32_t init() {
    const limine_file* kf = g_boot_info.kernel_file;
    if (!kf) {
        log::warn("symtab: kernel file not available");
        return ERR_NO_FILE;
    }

    ensure_mapped(reinterpret_cast<uintptr_t>(kf), sizeof(limine_file));

    if (!kf->address || kf->size == 0) {
        log::warn("symtab: kernel file has no data");
        return ERR_NO_FILE;
    }

    uintptr_t file_virt = reinterpret_cast<uintptr_t>(kf->address);
    ensure_mapped(file_virt, kf->size);

    auto* base = reinterpret_cast<const uint8_t*>(kf->address);
    auto* ehdr = reinterpret_cast<const elf64::Ehdr*>(base);

    if (ehdr->e_ident[0] != elf64::ELFMAG0 ||
        ehdr->e_ident[1] != elf64::ELFMAG1 ||
        ehdr->e_ident[2] != elf64::ELFMAG2 ||
        ehdr->e_ident[3] != elf64::ELFMAG3 ||
        ehdr->e_ident[4] != elf64::ELFCLASS64) {
        log::warn("symtab: invalid ELF header");
        return ERR_BAD_ELF;
    }

    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        log::warn("symtab: no section headers");
        return ERR_BAD_ELF;
    }

    uint64_t shdr_end = ehdr->e_shoff + static_cast<uint64_t>(ehdr->e_shnum) * sizeof(elf64::Shdr);
    if (shdr_end > kf->size) {
        log::warn("symtab: section headers extend beyond kernel file");
        return ERR_BAD_ELF;
    }

    auto* shdr_base = reinterpret_cast<const elf64::Shdr*>(base + ehdr->e_shoff);

    const elf64::Shdr* symtab_shdr = nullptr;
    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        if (shdr_base[i].sh_type == elf64::SHT_SYMTAB) {
            symtab_shdr = &shdr_base[i];
            break;
        }
    }

    if (!symtab_shdr) {
        log::warn("symtab: no .symtab section found");
        return ERR_NO_SYMTAB;
    }

    if (symtab_shdr->sh_link == 0 || symtab_shdr->sh_link >= ehdr->e_shnum) {
        log::warn("symtab: .symtab has invalid strtab link");
        return ERR_BAD_ELF;
    }

    const elf64::Shdr* strtab_shdr = &shdr_base[symtab_shdr->sh_link];
    if (strtab_shdr->sh_type != elf64::SHT_STRTAB) {
        log::warn("symtab: linked section is not a string table");
        return ERR_BAD_ELF;
    }

    uint64_t symtab_size = symtab_shdr->sh_size;
    uint64_t strtab_size = strtab_shdr->sh_size;
    uint64_t sym_count = symtab_size / sizeof(elf64::Sym);

    if (symtab_shdr->sh_offset + symtab_size > kf->size ||
        strtab_shdr->sh_offset + strtab_size > kf->size) {
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

    string::memcpy(sym_copy, base + symtab_shdr->sh_offset, symtab_size);
    string::memcpy(str_copy, base + strtab_shdr->sh_offset, strtab_size);

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
