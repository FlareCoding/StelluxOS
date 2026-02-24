#include "debug/debug.h"
#include "debug/kernel_elf.h"
#include "debug/symtab.h"
#include "debug/dwarf_line.h"
#include "boot/boot_services.h"
#include "common/logging.h"
#include "mm/paging.h"

namespace debug {

__PRIVILEGED_CODE static void ensure_mapped(uintptr_t virt, uint64_t size) {
    uintptr_t hhdm = g_boot_info.hhdm_offset;
    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    uintptr_t page_start = virt & ~(paging::PAGE_SIZE_4KB - 1);
    uintptr_t page_end   = (virt + size + paging::PAGE_SIZE_4KB - 1)
                           & ~(paging::PAGE_SIZE_4KB - 1);

    for (uintptr_t v = page_start; v < page_end; v += paging::PAGE_SIZE_4KB) {
        if (paging::is_mapped(v, root)) continue;
        pmm::phys_addr_t phys = v - hhdm;
        paging::map_page(v, phys, paging::PAGE_KERNEL_RO, root);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    const limine_file* kf = g_boot_info.kernel_file;
    if (!kf) {
        log::warn("debug: kernel file not available");
        return OK;
    }

    ensure_mapped(reinterpret_cast<uintptr_t>(kf), sizeof(limine_file));

    if (!kf->address || kf->size == 0) {
        log::warn("debug: kernel file has no data");
        return OK;
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
        log::warn("debug: invalid ELF header");
        return OK;
    }

    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        log::warn("debug: no section headers");
        return OK;
    }

    uint64_t shdr_end = ehdr->e_shoff
        + static_cast<uint64_t>(ehdr->e_shnum) * sizeof(elf64::Shdr);
    if (shdr_end > kf->size) {
        log::warn("debug: section headers extend beyond kernel file");
        return OK;
    }

    auto* shdr_base = reinterpret_cast<const elf64::Shdr*>(base + ehdr->e_shoff);

    const char* shstrtab = nullptr;
    uint64_t shstrtab_size = 0;
    if (ehdr->e_shstrndx != 0 && ehdr->e_shstrndx < ehdr->e_shnum) {
        const elf64::Shdr* strtab_sec = &shdr_base[ehdr->e_shstrndx];
        if (strtab_sec->sh_offset + strtab_sec->sh_size <= kf->size) {
            shstrtab = reinterpret_cast<const char*>(base + strtab_sec->sh_offset);
            shstrtab_size = strtab_sec->sh_size;
        }
    }

    kernel_elf elf;
    elf.base          = base;
    elf.ehdr          = ehdr;
    elf.shdr_base     = shdr_base;
    elf.shstrtab      = shstrtab;
    elf.shstrtab_size = shstrtab_size;
    elf.file_size     = kf->size;
    elf.shnum         = ehdr->e_shnum;

    int32_t rc = symtab::init(elf);
    if (rc != symtab::OK) {
        log::warn("debug: symbol table unavailable (error %d)", rc);
    }

    rc = dwarf_line::init(elf);
    if (rc != dwarf_line::OK) {
        log::warn("debug: DWARF line info unavailable (error %d)", rc);
    }

    return OK;
}

} // namespace debug
