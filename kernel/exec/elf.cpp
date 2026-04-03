#include "exec/elf.h"
#include "exec/elf64.h"
#include "exec/elf_arch.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"
#include "hw/cache.h"

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
    out->e_phoff   = ehdr->e_phoff;
    out->phentsize = ehdr->e_phentsize;
    out->phnum     = ehdr->e_phnum;
    return OK;
}

int32_t parse_elf(const char* path, elf_image* out, fs::node* base_dir) {
    fs::file* f = fs::open_at(base_dir, path, fs::O_RDONLY);
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
    const void* buffer,
    size_t buffer_size,
    const elf_image& img,
    uint64_t pt_root
) {
    auto* base = static_cast<const uint8_t*>(buffer);

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

                if (file_offset + copy_len <= buffer_size) {
                    string::memcpy(page_ptr + page_offset, base + file_offset, copy_len);
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

    mm::mm_context* mm_ctx = nullptr;
    int32_t load_rc = OK;

    RUN_ELEVATED({
        mm_ctx = mm::mm_context_create();
        if (!mm_ctx) {
            load_rc = ERR_PT_CREATE;
        } else {
            load_rc = load_segments(buffer, size, img, mm_ctx->pt_root);
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

int32_t load_elf(const char* path, loaded_image* out, fs::node* base_dir) {
    fs::file* f = fs::open_at(base_dir, path, fs::O_RDONLY);
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

    int32_t result = load_elf(buffer, attr.size, out);

    heap::ufree(buffer);
    fs::close(f);
    return result;
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
