#include "exec/elf.h"
#include "exec/elf64.h"
#include "exec/elf_arch.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "common/string.h"
#include "common/logging.h"

namespace exec {

int32_t parse_elf(const void* buffer, size_t size, elf_image* out) {
    if (size < sizeof(elf64::Ehdr)) {
        return ERR_INVALID_MAGIC;
    }

    auto* ehdr = static_cast<const elf64::Ehdr*>(buffer);

    if (ehdr->e_ident[0] != elf64::ELFMAG0 ||
        ehdr->e_ident[1] != elf64::ELFMAG1 ||
        ehdr->e_ident[2] != elf64::ELFMAG2 ||
        ehdr->e_ident[3] != elf64::ELFMAG3) {
        return ERR_INVALID_MAGIC;
    }

    if (ehdr->e_ident[elf64::EI_CLASS] != elf64::ELFCLASS64) {
        return ERR_INVALID_CLASS;
    }

    if (ehdr->e_ident[elf64::EI_DATA] != elf64::ELFDATA2LSB) {
        return ERR_INVALID_DATA;
    }

    if (ehdr->e_ident[elf64::EI_VERSION] != elf64::EV_CURRENT) {
        return ERR_INVALID_VERSION;
    }

    if (ehdr->e_type != elf64::ET_EXEC) {
        return ERR_INVALID_TYPE;
    }

    if (ehdr->e_machine != ELF_EXPECTED_MACHINE) {
        return ERR_INVALID_ARCH;
    }

    if (ehdr->e_phentsize != sizeof(elf64::Phdr)) {
        return ERR_INVALID_PHDR;
    }

    uint64_t ph_end = ehdr->e_phoff +
        static_cast<uint64_t>(ehdr->e_phnum) * sizeof(elf64::Phdr);
    if (ph_end > size) {
        return ERR_INVALID_PHDR;
    }

    auto* phdrs = reinterpret_cast<const elf64::Phdr*>(
        static_cast<const uint8_t*>(buffer) + ehdr->e_phoff);

    out->segment_count = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const auto& ph = phdrs[i];
        if (ph.p_type != elf64::PT_LOAD) {
            continue;
        }

        if (ph.p_filesz > ph.p_memsz) {
            return ERR_INVALID_PHDR;
        }

        if (out->segment_count >= MAX_ELF_SEGMENTS) {
            return ERR_TOO_MANY_SEGMENTS;
        }

        auto& seg = out->segments[out->segment_count++];
        seg.vaddr  = ph.p_vaddr;
        seg.offset = ph.p_offset;
        seg.filesz = ph.p_filesz;
        seg.memsz  = ph.p_memsz;
        seg.align  = ph.p_align;
        seg.flags  = ph.p_flags;
    }

    if (out->segment_count == 0) {
        return ERR_NO_LOADABLE;
    }

    out->entry_point = ehdr->e_entry;
    return OK;
}

int32_t parse_elf(const char* path, elf_image* out) {
    fs::file* f = fs::open(path, fs::O_RDONLY);
    if (!f) {
        return ERR_FILE_OPEN;
    }

    fs::vattr attr;
    if (fs::fstat(f, &attr) != fs::OK) {
        fs::close(f);
        return ERR_FILE_READ;
    }

    void* buffer = heap::ualloc(attr.size);
    if (!buffer) {
        fs::close(f);
        return ERR_NO_MEM;
    }

    ssize_t n = fs::read(f, buffer, attr.size);
    if (n < 0 || static_cast<size_t>(n) != attr.size) {
        heap::ufree(buffer);
        fs::close(f);
        return ERR_FILE_READ;
    }

    int32_t result = parse_elf(buffer, attr.size, out);

    heap::ufree(buffer);
    fs::close(f);
    return result;
}

} // namespace exec
