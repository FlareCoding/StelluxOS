/*
 * Virtual Memory Manager.
 *
 * Stateless orchestration layer that combines KVA (VA bookkeeping),
 * PMM (physical pages), and paging (page tables) into complete
 * "allocate mapped kernel memory" operations. Has no internal tracking
 * tree — all persistent state lives in the layers below.
 */

#include "mm/vmm.h"
#include "mm/kva.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "common/utils/logging.h"
#include "common/utils/memory.h"

namespace vmm {

__PRIVILEGED_DATA static pmm::phys_addr_t g_kernel_root = 0;
__PRIVILEGED_DATA static bool g_initialized = false;

static inline uintptr_t align_up(uintptr_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

static inline uintptr_t align_down(uintptr_t val, size_t align) {
    return val & ~(align - 1);
}

static inline bool must_zero(paging::page_flags_t flags, uint32_t alloc_flags) {
    return (alloc_flags & ALLOC_ZERO) || (flags & paging::PAGE_USER);
}

// Translate KVA error codes to VMM error codes.
__PRIVILEGED_CODE static int32_t translate_kva_error(int32_t kva_err) {
    switch (kva_err) {
        case kva::ERR_NO_VIRT:     return ERR_NO_VIRT;
        case kva::ERR_NO_MEM:      return ERR_NO_MEM;
        case kva::ERR_NOT_FOUND:   return ERR_NOT_FOUND;
        default:                   return ERR_INVALID_ARG;
    }
}

// Rollback: unmap and free physical for [base, base + mapped_bytes).
// For non-contiguous allocations only (each page freed individually).
__PRIVILEGED_CODE static void rollback_non_contiguous(
    uintptr_t base, size_t mapped_bytes
) {
    for (size_t off = 0; off < mapped_bytes; off += pmm::PAGE_SIZE) {
        uintptr_t virt = base + off;
        pmm::phys_addr_t phys = paging::get_physical(virt, g_kernel_root);
        paging::unmap_page(virt, g_kernel_root);
        if (phys != 0) {
            pmm::free_page(phys);
        }
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    g_kernel_root = paging::get_kernel_pt_root();
    g_initialized = true;
    return OK;
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc(
    size_t               pages,
    paging::page_flags_t flags,
    uint32_t             alloc_flags,
    kva::tag             tag,
    uintptr_t&           out
) {
    if (pages == 0) {
        return ERR_INVALID_ARG;
    }
    if (!(flags & paging::PAGE_READ)) {
        return ERR_INVALID_ARG;
    }

    size_t total_bytes = pages * pmm::PAGE_SIZE;
    bool zero = must_zero(flags, alloc_flags);

    kva::allocation kva_out;
    int32_t rc = kva::alloc(total_bytes, pmm::PAGE_SIZE, 0, 0,
                            kva::placement::low, tag, 0, kva_out);
    if (rc != kva::OK) {
        return translate_kva_error(rc);
    }

    uintptr_t base = kva_out.base;
    size_t mapped_bytes = 0;

    for (size_t i = 0; i < pages; i++) {
        pmm::phys_addr_t phys = pmm::alloc_page();
        if (phys == 0) {
            rollback_non_contiguous(base, mapped_bytes);
            kva::free(base);
            return ERR_NO_PHYS;
        }

        if (zero) {
            memory::memset(paging::phys_to_virt(phys), 0, pmm::PAGE_SIZE);
        }

        rc = paging::map_page(base + i * pmm::PAGE_SIZE, phys, flags, g_kernel_root);
        if (rc != paging::OK) {
            pmm::free_page(phys);
            rollback_non_contiguous(base, mapped_bytes);
            kva::free(base);
            return ERR_PAGING;
        }

        mapped_bytes += pmm::PAGE_SIZE;
    }

    out = base;
    return OK;
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc_contiguous(
    size_t               pages,
    pmm::zone_mask_t     zone,
    paging::page_flags_t flags,
    uint32_t             alloc_flags,
    kva::tag             tag,
    uintptr_t&           out_addr,
    pmm::phys_addr_t&    out_phys
) {
    if (pages == 0) {
        return ERR_INVALID_ARG;
    }

    uint8_t order = pmm::pages_to_order(pages);
    size_t actual_pages = pmm::order_to_pages(order);
    size_t actual_bytes = actual_pages * pmm::PAGE_SIZE;
    bool zero = must_zero(flags, alloc_flags);

    // Alignment: only elevated for large page mapping, otherwise 4KB
    size_t align = pmm::PAGE_SIZE;
    if ((alloc_flags & ALLOC_ALLOW_1GB) && order >= pmm::ORDER_1GB) {
        align = paging::PAGE_SIZE_1GB;
    } else if ((alloc_flags & ALLOC_ALLOW_2MB) && order >= pmm::ORDER_2MB) {
        align = paging::PAGE_SIZE_2MB;
    }

    kva::allocation kva_out;
    int32_t rc = kva::alloc(actual_bytes, align, 0, 0,
                            kva::placement::low, tag, order, kva_out);
    if (rc != kva::OK) {
        return translate_kva_error(rc);
    }

    uintptr_t base = kva_out.base;

    pmm::phys_addr_t phys = pmm::alloc_pages(order, zone);
    if (phys == 0) {
        kva::free(base);
        return ERR_NO_PHYS;
    }

    if (zero) {
        memory::memset(paging::phys_to_virt(phys), 0, actual_bytes);
    }

    // Map using large pages when aligned and allowed
    uintptr_t virt = base;
    pmm::phys_addr_t pa = phys;
    size_t remaining = actual_bytes;

    while (remaining > 0) {
        paging::page_flags_t map_flags = flags;
        size_t step = pmm::PAGE_SIZE;

        if ((alloc_flags & ALLOC_ALLOW_1GB) &&
            remaining >= paging::PAGE_SIZE_1GB &&
            (virt & (paging::PAGE_SIZE_1GB - 1)) == 0 &&
            (pa & (paging::PAGE_SIZE_1GB - 1)) == 0) {
            map_flags |= paging::PAGE_HUGE_1GB;
            step = paging::PAGE_SIZE_1GB;
        } else if ((alloc_flags & ALLOC_ALLOW_2MB) &&
                   remaining >= paging::PAGE_SIZE_2MB &&
                   (virt & (paging::PAGE_SIZE_2MB - 1)) == 0 &&
                   (pa & (paging::PAGE_SIZE_2MB - 1)) == 0) {
            map_flags |= paging::PAGE_LARGE_2MB;
            step = paging::PAGE_SIZE_2MB;
        }

        rc = paging::map_page(virt, pa, map_flags, g_kernel_root);
        if (rc != paging::OK) {
            // Rollback mapped portion
            uintptr_t rv = base;
            while (rv < virt) {
                paging::page_flags_t pf = paging::get_page_flags(rv, g_kernel_root);
                size_t rs = pmm::PAGE_SIZE;
                if (pf & paging::PAGE_HUGE_1GB) {
                    rs = paging::PAGE_SIZE_1GB;
                } else if (pf & paging::PAGE_LARGE_2MB) {
                    rs = paging::PAGE_SIZE_2MB;
                }
                paging::unmap_page(rv, g_kernel_root);
                rv += rs;
            }
            pmm::free_pages(phys, order);
            kva::free(base);
            return ERR_PAGING;
        }

        virt += step;
        pa += step;
        remaining -= step;
    }

    out_addr = base;
    out_phys = phys;
    return OK;
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc_stack(
    size_t     usable_pages,
    uint16_t   guard_pages,
    kva::tag   tag,
    uintptr_t& out_base,
    uintptr_t& out_top
) {
    if (usable_pages == 0) {
        return ERR_INVALID_ARG;
    }
    if (tag != kva::tag::privileged_stack && tag != kva::tag::unprivileged_stack) {
        return ERR_INVALID_ARG;
    }

    paging::page_flags_t flags = paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_NORMAL;
    if (tag == kva::tag::unprivileged_stack) {
        flags |= paging::PAGE_USER;
    }

    kva::allocation kva_out;
    int32_t rc = kva::alloc(usable_pages * pmm::PAGE_SIZE, pmm::PAGE_SIZE,
                            guard_pages, 0, kva::placement::high, tag, 0, kva_out);
    if (rc != kva::OK) {
        return translate_kva_error(rc);
    }

    uintptr_t base = kva_out.base;
    size_t mapped_bytes = 0;

    for (size_t i = 0; i < usable_pages; i++) {
        pmm::phys_addr_t phys = pmm::alloc_page();
        if (phys == 0) {
            rollback_non_contiguous(base, mapped_bytes);
            kva::free(base);
            return ERR_NO_PHYS;
        }

        // Stacks are always zeroed
        memory::memset(paging::phys_to_virt(phys), 0, pmm::PAGE_SIZE);

        rc = paging::map_page(base + i * pmm::PAGE_SIZE, phys, flags, g_kernel_root);
        if (rc != paging::OK) {
            pmm::free_page(phys);
            rollback_non_contiguous(base, mapped_bytes);
            kva::free(base);
            return ERR_PAGING;
        }

        mapped_bytes += pmm::PAGE_SIZE;
    }

    out_base = base;
    out_top = base + kva_out.size;
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t map_phys_internal(
    pmm::phys_addr_t     phys,
    size_t               size,
    paging::page_flags_t flags,
    kva::tag             tag,
    uintptr_t&           out_base,
    uintptr_t&           out_va
) {
    if (size == 0) {
        return ERR_INVALID_ARG;
    }
    if (phys + size < phys) {
        return ERR_INVALID_ARG; // overflow
    }

    uintptr_t phys_base = align_down(phys, pmm::PAGE_SIZE);
    size_t offset = phys - phys_base;
    size_t total = align_up(size + offset, pmm::PAGE_SIZE);

    if (phys_base + total < phys_base) {
        return ERR_INVALID_ARG; // page-aligned overflow
    }

    kva::allocation kva_out;
    int32_t rc = kva::alloc(total, pmm::PAGE_SIZE, 0, 0,
                            kva::placement::low, tag, 0, kva_out);
    if (rc != kva::OK) {
        return translate_kva_error(rc);
    }

    uintptr_t base = kva_out.base;
    size_t num_pages = total / pmm::PAGE_SIZE;

    for (size_t i = 0; i < num_pages; i++) {
        rc = paging::map_page(base + i * pmm::PAGE_SIZE,
                              phys_base + i * pmm::PAGE_SIZE,
                              flags, g_kernel_root);
        if (rc != paging::OK) {
            // Rollback — unmap only, do not free physical (it's hardware MMIO)
            for (size_t j = 0; j < i; j++) {
                paging::unmap_page(base + j * pmm::PAGE_SIZE, g_kernel_root);
            }
            kva::free(base);
            return ERR_PAGING;
        }
    }

    out_base = base;
    out_va = base + offset;
    return OK;
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t map_device(
    pmm::phys_addr_t     phys,
    size_t               size,
    paging::page_flags_t flags,
    uintptr_t&           out_base,
    uintptr_t&           out_va
) {
    if (!(flags & paging::PAGE_TYPE_MASK)) {
        flags |= paging::PAGE_DEVICE;
    }
    return map_phys_internal(phys, size, flags, kva::tag::mmio, out_base, out_va);
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t map_phys(
    pmm::phys_addr_t     phys,
    size_t               size,
    paging::page_flags_t flags,
    uintptr_t&           out_base,
    uintptr_t&           out_va
) {
    return map_phys_internal(phys, size, flags, kva::tag::phys_map, out_base, out_va);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t free(uintptr_t addr) {
    kva::allocation alloc;
    int32_t rc = kva::query(addr, alloc);
    if (rc != kva::OK) {
        return ERR_NOT_FOUND;
    }

    uintptr_t base = alloc.base;
    size_t size = alloc.size;
    kva::tag tag = alloc.alloc_tag;
    uint8_t order = alloc.pmm_order;

    // Free physical pages (before unmapping so get_physical still works)
    if (tag == kva::tag::mmio || tag == kva::tag::phys_map) {
        // Caller-provided physical — do not free
    } else if (order > 0) {
        // Contiguous: free the entire block at once
        pmm::phys_addr_t phys = paging::get_physical(base, g_kernel_root);
        if (phys != 0) {
            pmm::free_pages(phys, order);
        }
    } else {
        // Non-contiguous: free each 4KB page individually
        for (size_t off = 0; off < size; off += pmm::PAGE_SIZE) {
            pmm::phys_addr_t phys = paging::get_physical(base + off, g_kernel_root);
            if (phys != 0) {
                pmm::free_page(phys);
            }
        }
    }

    // Unmap: walk by detected page size for correctness with large pages
    uintptr_t pos = base;
    uintptr_t end_addr = base + size;
    while (pos < end_addr) {
        paging::page_flags_t pf = paging::get_page_flags(pos, g_kernel_root);
        size_t step = pmm::PAGE_SIZE;
        if (pf & paging::PAGE_HUGE_1GB) {
            step = paging::PAGE_SIZE_1GB;
        } else if (pf & paging::PAGE_LARGE_2MB) {
            step = paging::PAGE_SIZE_2MB;
        }

        paging::unmap_page(pos, g_kernel_root);
        pos += step;
    }

    // Flush TLB for the entire range
    paging::flush_tlb_range(base, base + size);

    // Free the KVA reservation (use alloc.base, not addr, for MMIO offset case)
    kva::free(alloc.base);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_state() {
    kva::dump_state();
}

} // namespace vmm
