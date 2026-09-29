#include "exec/elf.h"
#include "exec/elf64.h"
#include "exec/elf_arch.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"
#include "hw/cache.h"

namespace exec {

// Where segment bytes come from: an image already in memory, or a file read in place
struct segment_source {
    const uint8_t* buffer;
    fs::file*      file;
};

static int32_t check_elf_header(const elf64::Ehdr& ehdr) {
    if (ehdr.e_ident[0] != elf64::ELFMAG0 ||
        ehdr.e_ident[1] != elf64::ELFMAG1 ||
        ehdr.e_ident[2] != elf64::ELFMAG2 ||
        ehdr.e_ident[3] != elf64::ELFMAG3) {
        return ERR_INVALID_MAGIC;
    }

    if (ehdr.e_ident[elf64::EI_CLASS] != elf64::ELFCLASS64) {
        return ERR_INVALID_CLASS;
    }

    if (ehdr.e_ident[elf64::EI_DATA] != elf64::ELFDATA2LSB) {
        return ERR_INVALID_DATA;
    }

    if (ehdr.e_ident[elf64::EI_VERSION] != elf64::EV_CURRENT) {
        return ERR_INVALID_VERSION;
    }

    if (ehdr.e_type != elf64::ET_EXEC) {
        return ERR_INVALID_TYPE;
    }

    if (ehdr.e_machine != ELF_EXPECTED_MACHINE) {
        return ERR_INVALID_ARCH;
    }

    if (ehdr.e_phentsize != sizeof(elf64::Phdr)) {
        return ERR_INVALID_PHDR;
    }

    return OK;
}

static uint64_t program_header_bytes(const elf64::Ehdr& ehdr) {
    return static_cast<uint64_t>(ehdr.e_phnum) * sizeof(elf64::Phdr);
}

static bool range_fits(uint64_t offset, uint64_t len, uint64_t size) {
    return offset <= size && len <= size - offset;
}

static int32_t collect_segments(const elf64::Ehdr& ehdr, const elf64::Phdr* phdrs, uint64_t image_size,
                                elf_image* out) {
    out->segment_count = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        const auto& ph = phdrs[i];
        if (ph.p_type != elf64::PT_LOAD) {
            continue;
        }

        if (ph.p_filesz > ph.p_memsz) {
            return ERR_INVALID_PHDR;
        }

        // File bytes past the end of the image would load as zeroed code or data
        if (ph.p_filesz > 0 && !range_fits(ph.p_offset, ph.p_filesz, image_size)) {
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

    out->entry_point = ehdr.e_entry;
    out->e_phoff   = ehdr.e_phoff;
    out->phentsize = ehdr.e_phentsize;
    out->phnum     = ehdr.e_phnum;
    return OK;
}

static bool read_file_range(fs::file* f, uint64_t offset, void* dst, size_t len) {
    int64_t pos = static_cast<int64_t>(offset);
    if (fs::seek(f, pos, fs::SEEK_SET) != pos) {
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
        ssize_t n = fs::read(f, bytes + done, len - done);
        if (n <= 0) {
            return false;
        }

        done += static_cast<size_t>(n);
    }

    return true;
}

static bool read_segment_bytes(const segment_source& src, uint64_t offset, void* dst, size_t len) {
    if (src.buffer) {
        string::memcpy(dst, src.buffer + offset, len);
        return true;
    }

    return read_file_range(src.file, offset, dst, len);
}

int32_t parse_elf(const void* buffer, size_t size, elf_image* out) {
    if (size < sizeof(elf64::Ehdr)) {
        return ERR_INVALID_MAGIC;
    }

    auto* ehdr = static_cast<const elf64::Ehdr*>(buffer);
    int32_t rc = check_elf_header(*ehdr);
    if (rc != OK) {
        return rc;
    }

    if (!range_fits(ehdr->e_phoff, program_header_bytes(*ehdr), size)) {
        return ERR_INVALID_PHDR;
    }

    auto* phdrs = reinterpret_cast<const elf64::Phdr*>(static_cast<const uint8_t*>(buffer) + ehdr->e_phoff);
    return collect_segments(*ehdr, phdrs, size, out);
}

// Reads only the ELF header and the program header table, wherever the table sits
static int32_t parse_file_headers(fs::file* f, elf_image* out) {
    fs::vattr attr;
    if (fs::fstat(f, &attr) != fs::OK) {
        return ERR_FILE_READ;
    }

    if (attr.size < sizeof(elf64::Ehdr)) {
        return ERR_INVALID_MAGIC;
    }

    elf64::Ehdr ehdr;
    if (!read_file_range(f, 0, &ehdr, sizeof(ehdr))) {
        return ERR_FILE_READ;
    }

    int32_t rc = check_elf_header(ehdr);
    if (rc != OK) {
        return rc;
    }

    uint64_t table_bytes = program_header_bytes(ehdr);
    if (!range_fits(ehdr.e_phoff, table_bytes, attr.size)) {
        return ERR_INVALID_PHDR;
    }

    if (table_bytes == 0) {
        return ERR_NO_LOADABLE;
    }

    auto* phdrs = static_cast<elf64::Phdr*>(heap::ualloc(table_bytes));
    if (!phdrs) {
        return ERR_NO_MEM;
    }

    rc = read_file_range(f, ehdr.e_phoff, phdrs, table_bytes)
        ? collect_segments(ehdr, phdrs, attr.size, out)
        : ERR_FILE_READ;
    heap::ufree(phdrs);

    return rc;
}

// On success the file stays open so the caller can read segments from it
static int32_t open_and_parse(const char* path, fs::node* base_dir, fs::file** out_file, elf_image* out) {
    fs::file* f = fs::open_at(base_dir, path, fs::O_RDONLY);
    if (!f) {
        return ERR_FILE_OPEN;
    }

    int32_t rc = parse_file_headers(f, out);
    if (rc != OK) {
        fs::close(f);
        return rc;
    }

    *out_file = f;
    return OK;
}

int32_t parse_elf(const char* path, elf_image* out, fs::node* base_dir) {
    fs::file* f = nullptr;
    int32_t rc = open_and_parse(path, base_dir, &f, out);
    if (rc == OK) {
        fs::close(f);
    }

    return rc;
}

static paging::page_flags_t elf_flags_to_page_flags(uint32_t elf_flags) {
    paging::page_flags_t flags = paging::PAGE_USER;
    if (elf_flags & elf64::PF_R) flags |= paging::PAGE_READ;
    if (elf_flags & elf64::PF_W) flags |= paging::PAGE_WRITE;
    if (elf_flags & elf64::PF_X) flags |= paging::PAGE_EXEC;
    return flags;
}

static uint32_t elf_flags_to_vma_prot(uint32_t elf_flags) {
    uint32_t prot = 0;
    if (elf_flags & elf64::PF_R) prot |= mm::MM_PROT_READ;
    if (elf_flags & elf64::PF_W) prot |= mm::MM_PROT_WRITE;
    if (elf_flags & elf64::PF_X) prot |= mm::MM_PROT_EXEC;
    return prot;
}

__PRIVILEGED_CODE static int32_t load_segments(
    const segment_source& src,
    const elf_image& img,
    uint64_t pt_root
) {
    for (uint32_t i = 0; i < img.segment_count; i++) {
        const auto& seg = img.segments[i];

        uint64_t vaddr_start = pmm::page_align_down(seg.vaddr);
        uint64_t vaddr_end   = pmm::page_align_up(seg.vaddr + seg.memsz);
        size_t num_pages     = (vaddr_end - vaddr_start) / pmm::PAGE_SIZE;

        paging::page_flags_t flags = elf_flags_to_page_flags(seg.flags);

        uint64_t seg_file_start = seg.offset;
        uint64_t seg_vaddr      = seg.vaddr;

        for (size_t p = 0; p < num_pages; p++) {
            uint64_t page_vaddr = vaddr_start + p * pmm::PAGE_SIZE;

            pmm::phys_addr_t phys;
            uint8_t* page_ptr;
            bool already_mapped = paging::is_mapped(page_vaddr, pt_root);

            if (already_mapped) {
                phys = paging::get_physical(page_vaddr, pt_root);
                page_ptr = static_cast<uint8_t*>(paging::phys_to_virt(phys));
            } else {
                phys = pmm::alloc_page();
                if (phys == 0) {
                    return ERR_PAGE_ALLOC;
                }

                page_ptr = static_cast<uint8_t*>(paging::phys_to_virt(phys));
                string::memset(page_ptr, 0, pmm::PAGE_SIZE);

                int32_t rc = paging::map_page(page_vaddr, phys, flags, pt_root);
                if (rc != paging::OK) {
                    pmm::free_page(phys);
                    return ERR_PAGE_MAP;
                }
            }

            uint64_t copy_start = (page_vaddr < seg_vaddr) ? seg_vaddr : page_vaddr;
            uint64_t page_end = page_vaddr + pmm::PAGE_SIZE;
            uint64_t data_end = seg_vaddr + seg.filesz;
            uint64_t copy_end = (page_end < data_end) ? page_end : data_end;

            if (copy_start < copy_end) {
                uint64_t file_offset = seg_file_start + (copy_start - seg_vaddr);
                size_t copy_len = copy_end - copy_start;
                size_t page_offset = copy_start - page_vaddr;

                if (!read_segment_bytes(src, file_offset, page_ptr + page_offset, copy_len)) {
                    return ERR_FILE_READ;
                }
            }

            // Flush I-cache for executable pages so the CPU doesn't execute
            // stale instructions from the I-cache (required on AArch64)
            if (seg.flags & elf64::PF_X) {
                cache::flush_icache_range(
                    reinterpret_cast<uintptr_t>(page_ptr), pmm::PAGE_SIZE);
            }
        }
    }

    return OK;
}

__PRIVILEGED_CODE static void cleanup_mapped_segment_pages(
    mm::mm_context* mm_ctx,
    const elf_image& img
) {
    if (!mm_ctx || mm_ctx->pt_root == 0) {
        return;
    }

    for (uint32_t i = 0; i < img.segment_count; i++) {
        const auto& seg = img.segments[i];
        uintptr_t seg_start = pmm::page_align_down(seg.vaddr);
        uintptr_t seg_end = pmm::page_align_up(seg.vaddr + seg.memsz);

        for (uintptr_t vaddr = seg_start; vaddr < seg_end; vaddr += pmm::PAGE_SIZE) {
            if (!paging::is_mapped(vaddr, mm_ctx->pt_root)) {
                continue;
            }

            pmm::phys_addr_t phys = paging::get_physical(vaddr, mm_ctx->pt_root);
            paging::unmap_page(vaddr, mm_ctx->pt_root);
            if (phys != 0) {
                pmm::free_page(phys);
            }
        }
    }
}

static int32_t load_image(const segment_source& src, const elf_image& img, loaded_image* out) {
    mm::mm_context* mm_ctx = nullptr;
    int32_t load_rc = OK;

    RUN_ELEVATED({
        mm_ctx = mm::mm_context_create();
        if (!mm_ctx) {
            load_rc = ERR_PT_CREATE;
        } else {
            load_rc = load_segments(src, img, mm_ctx->pt_root);
            if (load_rc != OK) {
                cleanup_mapped_segment_pages(mm_ctx, img);
                mm::mm_context_release(mm_ctx);
                mm_ctx = nullptr;
            } else {
                for (uint32_t i = 0; i < img.segment_count; i++) {
                    const auto& seg = img.segments[i];
                    uintptr_t seg_start = pmm::page_align_down(seg.vaddr);
                    uintptr_t seg_end = pmm::page_align_up(seg.vaddr + seg.memsz);
                    uint32_t prot = elf_flags_to_vma_prot(seg.flags);
                    uint32_t flags = mm::VMA_FLAG_PRIVATE | mm::VMA_FLAG_ELF;

                    int32_t rc = mm::mm_context_add_vma(
                        mm_ctx,
                        seg_start,
                        seg_end - seg_start,
                        prot ? prot : mm::MM_PROT_READ,
                        flags
                    );
                    if (rc != mm::MM_CTX_OK) {
                        cleanup_mapped_segment_pages(mm_ctx, img);
                        mm::mm_context_release(mm_ctx);
                        mm_ctx = nullptr;
                        load_rc = ERR_PAGE_MAP;
                        break;
                    }
                }
            }
        }
    });

    if (load_rc != OK) {
        return load_rc;
    }

    out->entry_point = img.entry_point;
    out->pt_root = mm_ctx->pt_root;
    out->mm_ctx = mm_ctx;
    out->segment_count = img.segment_count;
    out->phentsize = img.phentsize;
    out->phnum = img.phnum;

    out->phdr_vaddr = 0;
    if (img.segment_count > 0) {
        const auto& first = img.segments[0];
        uint64_t phdr_end = img.e_phoff + static_cast<uint64_t>(img.phnum) * img.phentsize;
        if (img.e_phoff >= first.offset &&
            phdr_end <= first.offset + first.filesz) {
            out->phdr_vaddr = first.vaddr + (img.e_phoff - first.offset);
        }
    }

    return OK;
}

int32_t load_elf(const void* buffer, size_t size, loaded_image* out) {
    if (!out) {
        return ERR_INVALID_PHDR;
    }

    out->mm_ctx = nullptr;
    out->pt_root = 0;

    elf_image img;
    int32_t rc = parse_elf(buffer, size, &img);
    if (rc != OK) {
        return rc;
    }

    return load_image(segment_source{static_cast<const uint8_t*>(buffer), nullptr}, img, out);
}

int32_t load_elf(const char* path, loaded_image* out, fs::node* base_dir) {
    if (!out) {
        return ERR_INVALID_PHDR;
    }

    out->mm_ctx = nullptr;
    out->pt_root = 0;

    elf_image img;
    fs::file* f = nullptr;
    int32_t rc = open_and_parse(path, base_dir, &f, &img);
    if (rc != OK) {
        return rc;
    }

    rc = load_image(segment_source{nullptr, f}, img, out);
    fs::close(f);

    return rc;
}

void unload_elf(loaded_image* img) {
    if (!img) {
        return;
    }

    if (img->mm_ctx) {
        RUN_ELEVATED({
            mm::mm_context_release(img->mm_ctx);
        });
    }
    img->mm_ctx = nullptr;
    img->pt_root = 0;
}

} // namespace exec
