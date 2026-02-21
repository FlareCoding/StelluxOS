#include "mm/paging.h"
#include "mm/paging_arch.h"
#include "mm/pmm.h"
#include "cpu/features.h"
#include "boot/boot_services.h"
#include "common/logging.h"
#include "common/string.h"
#include "sync/spinlock.h"

// Linker symbols for kernel boundaries
extern "C" {
    extern char __stlx_kern_start[];
    extern char __stlx_kern_priv_start[];
    extern char __stlx_kern_priv_end[];
    extern char __stlx_kern_end[];
    extern char __percpu_start[];
    extern char __percpu_end[];
    extern char __rodata_start[];
    extern char __rodata_end[];
    extern char __priv_rodata_start[];
    extern char __priv_data_start[];
}

namespace paging {

// Tracks whether paging has been initialized
__PRIVILEGED_DATA static bool g_initialized = false;
__PRIVILEGED_DATA static sync::spinlock g_pt_lock = sync::SPINLOCK_INIT;

__PRIVILEGED_CODE pmm::phys_addr_t get_kernel_pt_root() {
    // Read CR3 directly - the physical address is in bits 12-51
    // Since page tables are 4KB aligned, low 12 bits are zero/flags
    return read_cr3() & ~0xFFFULL;
}

__PRIVILEGED_CODE void set_kernel_pt_root(pmm::phys_addr_t root_pt) {
    write_cr3(root_pt);
}

__PRIVILEGED_CODE void* phys_to_virt(pmm::phys_addr_t phys) {
    return reinterpret_cast<void*>(phys + g_boot_info.hhdm_offset);
}

// Allocate a zeroed page for page tables
__PRIVILEGED_CODE static pmm::phys_addr_t alloc_table_page() {
    pmm::phys_addr_t phys;
    
    if (!g_initialized) {
        phys = pmm::bootstrap_allocator::alloc_page();
        if (phys == 0) {
            log::fatal("paging: bootstrap allocator exhausted");
        }
    } else {
        phys = pmm::alloc_page();
        if (phys == 0) {
            log::fatal("paging: out of memory for page tables");
        }
        void* virt = phys_to_virt(phys);
        string::memset(virt, 0, PAGE_SIZE_4KB);
    }
    
    return phys;
}

// Convert abstract flags to x86_64 PTE bits
__PRIVILEGED_CODE static pte_t flags_to_pte(pmm::phys_addr_t phys, page_flags_t flags) {
    pte_t pte = {};
    pte.present = 1;
    pte.phys_addr = phys >> 12;

    if (flags & PAGE_WRITE) {
        pte.read_write = 1;
    }
    if (flags & PAGE_USER) {
        pte.user_supervisor = 1;
    }
    if (!(flags & PAGE_EXEC)) {
        pte.execute_disable = 1;
    }
    if (flags & PAGE_GLOBAL) {
        pte.global = 1;
    }

    // Memory type handling via PAT/PCD/PWT
    uint32_t mem_type = flags & PAGE_TYPE_MASK;
    if (mem_type == PAGE_DEVICE) {
        // Uncached: PCD=1, PWT=0, PAT=0
        pte.page_cache_disable = 1;
    } else if (mem_type == PAGE_WC) {
        // Write-combining: Depends on PAT configuration
        // Assuming PAT index 1 is WC: PAT=0, PCD=0, PWT=1
        pte.page_write_through = 1;
    }
    // PAGE_NORMAL: PCD=0, PWT=0, PAT=0 (default, write-back)

    return pte;
}

// Convert PTE to abstract flags
__PRIVILEGED_CODE static page_flags_t pte_to_flags(const pte_t& pte) {
    if (!pte.present) {
        return 0;
    }

    page_flags_t flags = PAGE_READ;

    if (pte.read_write) {
        flags |= PAGE_WRITE;
    }
    if (pte.user_supervisor) {
        flags |= PAGE_USER;
    }
    if (!pte.execute_disable) {
        flags |= PAGE_EXEC;
    }
    if (pte.global) {
        flags |= PAGE_GLOBAL;
    }

    // Memory type
    if (pte.page_cache_disable) {
        flags |= PAGE_DEVICE;
    } else if (pte.page_write_through) {
        flags |= PAGE_WC;
    }

    return flags;
}

// Convert abstract flags to x86_64 2MB large page PDE
__PRIVILEGED_CODE static pde_2mb_t flags_to_pde_2mb(pmm::phys_addr_t phys, page_flags_t flags) {
    pde_2mb_t pde = {};
    pde.present = 1;
    pde.page_size = 1;
    pde.phys_addr = phys >> 21;

    if (flags & PAGE_WRITE) {
        pde.read_write = 1;
    }
    if (flags & PAGE_USER) {
        pde.user_supervisor = 1;
    }
    if (!(flags & PAGE_EXEC)) {
        pde.execute_disable = 1;
    }
    if (flags & PAGE_GLOBAL) {
        pde.global = 1;
    }

    // Memory type via PAT/PCD/PWT (PAT bit at position 12 for large pages)
    uint32_t mem_type = flags & PAGE_TYPE_MASK;
    if (mem_type == PAGE_DEVICE) {
        pde.page_cache_disable = 1;
    } else if (mem_type == PAGE_WC) {
        pde.page_write_through = 1;
    }

    return pde;
}

// Convert 2MB PDE to abstract flags
__PRIVILEGED_CODE static page_flags_t pde_2mb_to_flags(const pde_2mb_t& pde) {
    if (!pde.present) {
        return 0;
    }

    page_flags_t flags = PAGE_READ | PAGE_LARGE_2MB;

    if (pde.read_write) {
        flags |= PAGE_WRITE;
    }
    if (pde.user_supervisor) {
        flags |= PAGE_USER;
    }
    if (!pde.execute_disable) {
        flags |= PAGE_EXEC;
    }
    if (pde.global) {
        flags |= PAGE_GLOBAL;
    }

    if (pde.page_cache_disable) {
        flags |= PAGE_DEVICE;
    } else if (pde.page_write_through) {
        flags |= PAGE_WC;
    }

    return flags;
}

// Convert abstract flags to x86_64 1GB huge page PDPTE
__PRIVILEGED_CODE static pdpte_1gb_t flags_to_pdpte_1gb(pmm::phys_addr_t phys, page_flags_t flags) {
    pdpte_1gb_t pdpte = {};
    pdpte.present = 1;
    pdpte.page_size = 1;
    pdpte.phys_addr = phys >> 30;

    if (flags & PAGE_WRITE) {
        pdpte.read_write = 1;
    }
    if (flags & PAGE_USER) {
        pdpte.user_supervisor = 1;
    }
    if (!(flags & PAGE_EXEC)) {
        pdpte.execute_disable = 1;
    }
    if (flags & PAGE_GLOBAL) {
        pdpte.global = 1;
    }

    // Memory type via PAT/PCD/PWT (PAT bit at position 12 for huge pages)
    uint32_t mem_type = flags & PAGE_TYPE_MASK;
    if (mem_type == PAGE_DEVICE) {
        pdpte.page_cache_disable = 1;
    } else if (mem_type == PAGE_WC) {
        pdpte.page_write_through = 1;
    }

    return pdpte;
}

// Convert 1GB PDPTE to abstract flags
__PRIVILEGED_CODE static page_flags_t pdpte_1gb_to_flags(const pdpte_1gb_t& pdpte) {
    if (!pdpte.present) {
        return 0;
    }

    page_flags_t flags = PAGE_READ | PAGE_HUGE_1GB;

    if (pdpte.read_write) {
        flags |= PAGE_WRITE;
    }
    if (pdpte.user_supervisor) {
        flags |= PAGE_USER;
    }
    if (!pdpte.execute_disable) {
        flags |= PAGE_EXEC;
    }
    if (pdpte.global) {
        flags |= PAGE_GLOBAL;
    }

    if (pdpte.page_cache_disable) {
        flags |= PAGE_DEVICE;
    } else if (pdpte.page_write_through) {
        flags |= PAGE_WC;
    }

    return flags;
}

// Get or create page table at specified level
// Returns virtual address of table, creates if needed
__PRIVILEGED_CODE static void* get_or_create_table(pml4e_t* entry) {
    if (entry->present) {
        return phys_to_virt(entry->phys_addr << 12);
    }

    // Allocate new table
    pmm::phys_addr_t table_phys = alloc_table_page();

    // Set up entry pointing to new table
    entry->value = 0;
    entry->present = 1;
    entry->read_write = 1;
    entry->user_supervisor = 1;  // Allow user access to be controlled at leaf level
    entry->phys_addr = table_phys >> 12;

    return phys_to_virt(table_phys);
}

// Get pointer to PTE for a virtual address, creating tables as needed if create=true
// Returns nullptr if not mapped and create=false
__PRIVILEGED_CODE static pte_t* get_pte_ptr(pmm::phys_addr_t root_pt, virt_addr_t virt, bool create) {
    auto parts = split_virt_addr(virt);
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    // PML4 -> PDPT
    pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];
    if (!pml4e->present && !create) return nullptr;
    pdpt_t* pdpt = static_cast<pdpt_t*>(create ? get_or_create_table(pml4e) : phys_to_virt(pml4e->phys_addr << 12));
    if (!pdpt) return nullptr;

    // PDPT -> PD
    pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];
    if (pdpte->page_size) return nullptr;  // 1GB page, can't get PTE
    if (!pdpte->present && !create) return nullptr;
    page_directory_t* pd = static_cast<page_directory_t*>(create ? get_or_create_table(pdpte) : phys_to_virt(pdpte->phys_addr << 12));
    if (!pd) return nullptr;

    // PD -> PT
    pde_t* pde = &pd->entries[parts.pd_idx];
    if (pde->page_size) return nullptr;  // 2MB page, can't get PTE
    if (!pde->present && !create) return nullptr;
    page_table_t* pt = static_cast<page_table_t*>(create ? get_or_create_table(pde) : phys_to_virt(pde->phys_addr << 12));
    if (!pt) return nullptr;

    return &pt->entries[parts.pt_idx];
}

// Map a single 4KB page (internal, used during init before g_initialized is set)
__PRIVILEGED_CODE static int32_t map_page_4kb(pmm::phys_addr_t root_pt, virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags) {
    pte_t* pte = get_pte_ptr(root_pt, virt, true);
    if (!pte) {
        return ERR_INVALID_ADDR;
    }

    if (pte->present) {
        return ERR_ALREADY_MAPPED;
    }

    *pte = flags_to_pte(phys, flags);
    return OK;
}

// Map a single 2MB large page
__PRIVILEGED_CODE static int32_t map_page_2mb(pmm::phys_addr_t root_pt, virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags) {
    auto parts = split_virt_addr(virt);
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    // PML4 -> PDPT
    pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];
    pdpt_t* pdpt = static_cast<pdpt_t*>(get_or_create_table(pml4e));

    // PDPT -> PD (check for 1GB page conflict)
    pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];
    if (pdpte->present && pdpte->page_size) {
        return ERR_ALREADY_MAPPED;
    }
    page_directory_t* pd = static_cast<page_directory_t*>(get_or_create_table(pdpte));

    // Set PDE as 2MB large page (no PT level)
    pde_t* pde = &pd->entries[parts.pd_idx];
    if (pde->present) {
        // PDE exists — if it points to a PT (not a large page), check if
        // all 512 PTEs are empty. If so, free the PT and reclaim the entry.
        if (!pde->page_size) {
            auto* pt = static_cast<page_table_t*>(phys_to_virt(pde->phys_addr << 12));
            bool all_empty = true;
            for (int i = 0; i < 512; i++) {
                if (pt->entries[i].present) {
                    all_empty = false;
                    break;
                }
            }
            if (!all_empty) {
                return ERR_ALREADY_MAPPED;
            }
            pmm::phys_addr_t pt_phys = static_cast<pmm::phys_addr_t>(pde->phys_addr) << 12;
            pde->value = 0;
            pmm::free_page(pt_phys);
        } else {
            return ERR_ALREADY_MAPPED;
        }
    }

    pde_2mb_t large = flags_to_pde_2mb(phys, flags);
    *reinterpret_cast<pde_2mb_t*>(pde) = large;
    return OK;
}

// Map a single 1GB huge page
__PRIVILEGED_CODE static int32_t map_page_1gb(pmm::phys_addr_t root_pt, virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags) {
    if (!cpu::has(cpu::PAGE_1GB)) {
        return ERR_INVALID_FLAGS;
    }

    auto parts = split_virt_addr(virt);
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    // PML4 -> PDPT
    pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];
    pdpt_t* pdpt = static_cast<pdpt_t*>(get_or_create_table(pml4e));

    // Set PDPTE as 1GB huge page (no PD or PT levels)
    pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];
    if (pdpte->present) {
        if (!pdpte->page_size) {
            // PDPTE points to a PD — check if all 512 PDEs are empty
            auto* pd = static_cast<page_directory_t*>(phys_to_virt(pdpte->phys_addr << 12));
            bool all_empty = true;
            for (int i = 0; i < 512; i++) {
                if (pd->entries[i].present) {
                    all_empty = false;
                    break;
                }
            }
            if (!all_empty) {
                return ERR_ALREADY_MAPPED;
            }
            pmm::phys_addr_t pd_phys = static_cast<pmm::phys_addr_t>(pdpte->phys_addr) << 12;
            pdpte->value = 0;
            pmm::free_page(pd_phys);
        } else {
            return ERR_ALREADY_MAPPED;
        }
    }

    pdpte_1gb_t huge = flags_to_pdpte_1gb(phys, flags);
    *reinterpret_cast<pdpte_1gb_t*>(pdpte) = huge;
    return OK;
}

__PRIVILEGED_CODE static int32_t unmap_page_nolock(virt_addr_t virt, pmm::phys_addr_t root_pt);

__PRIVILEGED_CODE static int32_t map_page_nolock(virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return ERR_INVALID_ADDR;
    }

    // Handle unmap sentinel
    if (phys == PHYS_UNMAP) {
        return unmap_page_nolock(virt, root_pt);
    }

    // Route based on page size flags
    if (flags & PAGE_HUGE_1GB) {
        if ((virt & (PAGE_SIZE_1GB - 1)) != 0 || (phys & (PAGE_SIZE_1GB - 1)) != 0) {
            return ERR_ALIGNMENT;
        }
        return map_page_1gb(root_pt, virt, phys, flags);
    }

    if (flags & PAGE_LARGE_2MB) {
        if ((virt & (PAGE_SIZE_2MB - 1)) != 0 || (phys & (PAGE_SIZE_2MB - 1)) != 0) {
            return ERR_ALIGNMENT;
        }
        return map_page_2mb(root_pt, virt, phys, flags);
    }

    // 4KB page
    if ((virt & (PAGE_SIZE_4KB - 1)) != 0 || (phys & (PAGE_SIZE_4KB - 1)) != 0) {
        return ERR_ALIGNMENT;
    }

    return map_page_4kb(root_pt, virt, phys, flags);
}

__PRIVILEGED_CODE int32_t map_page(virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags, pmm::phys_addr_t root_pt) {
    sync::irq_lock_guard guard(g_pt_lock);
    return map_page_nolock(virt, phys, flags, root_pt);
}

__PRIVILEGED_CODE int32_t map_pages(virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags, size_t count, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return ERR_INVALID_ADDR;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    // Check alignment
    if ((virt & (PAGE_SIZE_4KB - 1)) != 0 || (phys & (PAGE_SIZE_4KB - 1)) != 0) {
        return ERR_ALIGNMENT;
    }

    // First pass: check that all pages are unmapped
    for (size_t i = 0; i < count; i++) {
        virt_addr_t v = virt + i * PAGE_SIZE_4KB;
        pte_t* pte = get_pte_ptr(root_pt, v, false);
        if (pte && pte->present) {
            return ERR_ALREADY_MAPPED;
        }
    }

    // Second pass: map all pages
    for (size_t i = 0; i < count; i++) {
        virt_addr_t v = virt + i * PAGE_SIZE_4KB;
        pmm::phys_addr_t p = phys + i * PAGE_SIZE_4KB;
        int32_t result = map_page_4kb(root_pt, v, p, flags);
        if (result != OK) {
            // This shouldn't happen since we checked, but handle it
            return result;
        }
    }

    return OK;
}

// Check if all 512 entries in a page table level are empty.
__PRIVILEGED_CODE static bool is_table_empty(const void* table) {
    const uint64_t* entries = static_cast<const uint64_t*>(table);
    for (int i = 0; i < 512; i++) {
        if (entries[i] != 0) return false;
    }
    return true;
}

// Free a page table page back to PMM if it was dynamically allocated.
// Bootstrap-allocated pages (PAGE_FLAG_RESERVED) are not freed.
__PRIVILEGED_CODE static void try_free_table_page(pmm::phys_addr_t phys) {
    auto* pfd = pmm::get_page_frame(phys);
    if (pfd && pfd->is_allocated()) {
        pmm::free_page(phys);
    }
}

__PRIVILEGED_CODE static int32_t unmap_page_nolock(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return OK;
    }

    auto parts = split_virt_addr(virt);
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];
    if (!pml4e->present) return OK;

    pdpt_t* pdpt = static_cast<pdpt_t*>(phys_to_virt(pml4e->phys_addr << 12));
    pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];
    if (!pdpte->present) return OK;

    // 1GB huge page
    if (pdpte->page_size) {
        pdpte->value = 0;
        flush_tlb_page(virt);
        if (is_table_empty(pdpt)) {
            pmm::phys_addr_t pdpt_phys = static_cast<pmm::phys_addr_t>(pml4e->phys_addr) << 12;
            pml4e->value = 0;
            try_free_table_page(pdpt_phys);
        }
        return OK;
    }

    page_directory_t* pd = static_cast<page_directory_t*>(phys_to_virt(pdpte->phys_addr << 12));
    pde_t* pde = &pd->entries[parts.pd_idx];
    if (!pde->present) return OK;

    // 2MB large page
    if (pde->page_size) {
        pde->value = 0;
        flush_tlb_page(virt);
        if (is_table_empty(pd)) {
            pmm::phys_addr_t pd_phys = static_cast<pmm::phys_addr_t>(pdpte->phys_addr) << 12;
            pdpte->value = 0;
            try_free_table_page(pd_phys);
            if (is_table_empty(pdpt)) {
                pmm::phys_addr_t pdpt_phys = static_cast<pmm::phys_addr_t>(pml4e->phys_addr) << 12;
                pml4e->value = 0;
                try_free_table_page(pdpt_phys);
            }
        }
        return OK;
    }

    // 4KB page
    page_table_t* pt = static_cast<page_table_t*>(phys_to_virt(pde->phys_addr << 12));
    pte_t* pte = &pt->entries[parts.pt_idx];
    if (!pte->present) return OK;

    pte->value = 0;
    flush_tlb_page(virt);

    // Cascade: reclaim empty page tables up the hierarchy
    if (is_table_empty(pt)) {
        pmm::phys_addr_t pt_phys = static_cast<pmm::phys_addr_t>(pde->phys_addr) << 12;
        pde->value = 0;
        try_free_table_page(pt_phys);
        if (is_table_empty(pd)) {
            pmm::phys_addr_t pd_phys = static_cast<pmm::phys_addr_t>(pdpte->phys_addr) << 12;
            pdpte->value = 0;
            try_free_table_page(pd_phys);
            if (is_table_empty(pdpt)) {
                pmm::phys_addr_t pdpt_phys = static_cast<pmm::phys_addr_t>(pml4e->phys_addr) << 12;
                pml4e->value = 0;
                try_free_table_page(pdpt_phys);
            }
        }
    }

    return OK;
}

__PRIVILEGED_CODE int32_t unmap_page(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    sync::irq_lock_guard guard(g_pt_lock);
    return unmap_page_nolock(virt, root_pt);
}

__PRIVILEGED_CODE int32_t unmap_pages(virt_addr_t virt, size_t count, pmm::phys_addr_t root_pt) {
    sync::irq_lock_guard guard(g_pt_lock);
    for (size_t i = 0; i < count; i++) {
        unmap_page_nolock(virt + i * PAGE_SIZE_4KB, root_pt);
    }
    return OK;
}

__PRIVILEGED_CODE int32_t set_page_flags(virt_addr_t virt, page_flags_t flags, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return ERR_NOT_MAPPED;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    auto parts = split_virt_addr(virt);
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];
    if (!pml4e->present) return ERR_NOT_MAPPED;

    pdpt_t* pdpt = static_cast<pdpt_t*>(phys_to_virt(pml4e->phys_addr << 12));
    pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];
    if (!pdpte->present) return ERR_NOT_MAPPED;

    // 1GB huge page
    if (pdpte->page_size) {
        auto* huge = reinterpret_cast<pdpte_1gb_t*>(pdpte);
        pmm::phys_addr_t phys = static_cast<pmm::phys_addr_t>(huge->phys_addr) << 30;
        *huge = flags_to_pdpte_1gb(phys, flags);
        flush_tlb_page(virt);
        return OK;
    }

    page_directory_t* pd = static_cast<page_directory_t*>(phys_to_virt(pdpte->phys_addr << 12));
    pde_t* pde = &pd->entries[parts.pd_idx];
    if (!pde->present) return ERR_NOT_MAPPED;

    // 2MB large page
    if (pde->page_size) {
        auto* large = reinterpret_cast<pde_2mb_t*>(pde);
        pmm::phys_addr_t phys = static_cast<pmm::phys_addr_t>(large->phys_addr) << 21;
        *large = flags_to_pde_2mb(phys, flags);
        flush_tlb_page(virt);
        return OK;
    }

    // 4KB page
    page_table_t* pt = static_cast<page_table_t*>(phys_to_virt(pde->phys_addr << 12));
    pte_t* pte = &pt->entries[parts.pt_idx];
    if (!pte->present) return ERR_NOT_MAPPED;

    pmm::phys_addr_t phys = static_cast<pmm::phys_addr_t>(pte->phys_addr) << 12;
    *pte = flags_to_pte(phys, flags);
    flush_tlb_page(virt);
    return OK;
}

__PRIVILEGED_CODE pmm::phys_addr_t get_physical(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return 0;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    auto parts = split_virt_addr(virt);
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];
    if (!pml4e->present) return 0;

    pdpt_t* pdpt = static_cast<pdpt_t*>(phys_to_virt(pml4e->phys_addr << 12));
    pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];
    if (!pdpte->present) return 0;

    // 1GB huge page
    if (pdpte->page_size) {
        auto* huge = reinterpret_cast<const pdpte_1gb_t*>(pdpte);
        pmm::phys_addr_t base = static_cast<pmm::phys_addr_t>(huge->phys_addr) << 30;
        return base + (virt & (PAGE_SIZE_1GB - 1));
    }

    page_directory_t* pd = static_cast<page_directory_t*>(phys_to_virt(pdpte->phys_addr << 12));
    pde_t* pde = &pd->entries[parts.pd_idx];
    if (!pde->present) return 0;

    // 2MB large page
    if (pde->page_size) {
        auto* large = reinterpret_cast<const pde_2mb_t*>(pde);
        pmm::phys_addr_t base = static_cast<pmm::phys_addr_t>(large->phys_addr) << 21;
        return base + (virt & (PAGE_SIZE_2MB - 1));
    }

    // 4KB page
    page_table_t* pt = static_cast<page_table_t*>(phys_to_virt(pde->phys_addr << 12));
    pte_t* pte = &pt->entries[parts.pt_idx];
    if (!pte->present) return 0;

    pmm::phys_addr_t base = static_cast<pmm::phys_addr_t>(pte->phys_addr) << 12;
    return base + (virt & (PAGE_SIZE_4KB - 1));
}

__PRIVILEGED_CODE page_flags_t get_page_flags(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return 0;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    auto parts = split_virt_addr(virt);
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];
    if (!pml4e->present) return 0;

    pdpt_t* pdpt = static_cast<pdpt_t*>(phys_to_virt(pml4e->phys_addr << 12));
    pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];
    if (!pdpte->present) return 0;

    // 1GB huge page
    if (pdpte->page_size) {
        return pdpte_1gb_to_flags(*reinterpret_cast<const pdpte_1gb_t*>(pdpte));
    }

    page_directory_t* pd = static_cast<page_directory_t*>(phys_to_virt(pdpte->phys_addr << 12));
    pde_t* pde = &pd->entries[parts.pd_idx];
    if (!pde->present) return 0;

    // 2MB large page
    if (pde->page_size) {
        return pde_2mb_to_flags(*reinterpret_cast<const pde_2mb_t*>(pde));
    }

    // 4KB page
    page_table_t* pt = static_cast<page_table_t*>(phys_to_virt(pde->phys_addr << 12));
    pte_t* pte = &pt->entries[parts.pt_idx];
    if (!pte->present) return 0;

    return pte_to_flags(*pte);
}

__PRIVILEGED_CODE bool is_mapped(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return false;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    auto parts = split_virt_addr(virt);
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];
    if (!pml4e->present) return false;

    pdpt_t* pdpt = static_cast<pdpt_t*>(phys_to_virt(pml4e->phys_addr << 12));
    pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];
    if (!pdpte->present) return false;
    if (pdpte->page_size) return true; // 1GB page

    page_directory_t* pd = static_cast<page_directory_t*>(phys_to_virt(pdpte->phys_addr << 12));
    pde_t* pde = &pd->entries[parts.pd_idx];
    if (!pde->present) return false;
    if (pde->page_size) return true; // 2MB page

    page_table_t* pt = static_cast<page_table_t*>(phys_to_virt(pde->phys_addr << 12));
    return pt->entries[parts.pt_idx].present;
}

__PRIVILEGED_CODE void flush_tlb_page(virt_addr_t virt) {
    invlpg(virt);
}

__PRIVILEGED_CODE void flush_tlb_range(virt_addr_t start, virt_addr_t end) {
    sync::irq_lock_guard guard(g_pt_lock);
    pmm::phys_addr_t root_pt = get_kernel_pt_root();
    virt_addr_t addr = start;

    while (addr < end) {
        size_t step = PAGE_SIZE_4KB;

        auto parts = split_virt_addr(addr);
        pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));
        pml4e_t* pml4e = &pml4->entries[parts.pml4_idx];

        if (pml4e->present) {
            pdpt_t* pdpt = static_cast<pdpt_t*>(phys_to_virt(pml4e->phys_addr << 12));
            pdpte_t* pdpte = &pdpt->entries[parts.pdpt_idx];

            if (pdpte->present && pdpte->page_size) {
                step = PAGE_SIZE_1GB;
            } else if (pdpte->present) {
                page_directory_t* pd = static_cast<page_directory_t*>(phys_to_virt(pdpte->phys_addr << 12));
                pde_t* pde = &pd->entries[parts.pd_idx];

                if (pde->present && pde->page_size) {
                    step = PAGE_SIZE_2MB;
                }
            }
        }

        invlpg(addr);
        addr += step;
    }
}

__PRIVILEGED_CODE void flush_tlb_all() {
    write_cr3(read_cr3());
}

__PRIVILEGED_CODE void dump_mappings() {
    if (!g_initialized) {
        log::info("paging: not initialized");
        return;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    pmm::phys_addr_t root_pt = get_kernel_pt_root();
    pml4_t* pml4 = static_cast<pml4_t*>(phys_to_virt(root_pt));

    log::info("paging: PML4 at phys=0x%lx", root_pt);

    uint64_t mapped_pages = 0;
    uint64_t mapped_2mb = 0;
    uint64_t mapped_1gb = 0;

    for (int pml4_idx = 0; pml4_idx < 512; pml4_idx++) {
        pml4e_t* pml4e = &pml4->entries[pml4_idx];
        if (!pml4e->present) continue;

        pdpt_t* pdpt = static_cast<pdpt_t*>(phys_to_virt(pml4e->phys_addr << 12));

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            pdpte_t* pdpte = &pdpt->entries[pdpt_idx];
            if (!pdpte->present) continue;

            if (pdpte->page_size) {
                mapped_1gb++;
                continue;
            }

            page_directory_t* pd = static_cast<page_directory_t*>(phys_to_virt(pdpte->phys_addr << 12));

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                pde_t* pde = &pd->entries[pd_idx];
                if (!pde->present) continue;

                if (pde->page_size) {
                    mapped_2mb++;
                    continue;
                }

                page_table_t* pt = static_cast<page_table_t*>(phys_to_virt(pde->phys_addr << 12));

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    pte_t* pte = &pt->entries[pt_idx];
                    if (pte->present) {
                        mapped_pages++;
                    }
                }
            }
        }
    }

    uint64_t total_mb = (mapped_pages * 4 + mapped_2mb * 2048 + mapped_1gb * 1024 * 1024) / 1024;
    log::info("paging: %lu 4KB pages, %lu 2MB pages, %lu 1GB pages (%lu MB total)",
              mapped_pages, mapped_2mb, mapped_1gb, total_mb);
}

__PRIVILEGED_CODE int32_t init() {
    if (g_initialized) {
        return OK;
    }

    // Allocate PML4 - this will be our new page table root
    pmm::phys_addr_t new_root = alloc_table_page();

    // Map HHDM region for all usable memory, using large/huge pages where aligned
    bool has_1gb_pages = cpu::has(cpu::PAGE_1GB);

    for (uint64_t i = 0; i < g_boot_info.memmap_entry_count; i++) {
        auto* entry = g_boot_info.memmap_entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        pmm::phys_addr_t phys = pmm::page_align_up(entry->base);
        pmm::phys_addr_t phys_end = pmm::page_align_down(entry->base + entry->length);

        if (phys_end <= phys) continue;

        while (phys < phys_end) {
            virt_addr_t virt = phys + g_boot_info.hhdm_offset;
            size_t remaining = phys_end - phys;

            // Try 1GB page if aligned and enough space
            if (has_1gb_pages
                && remaining >= PAGE_SIZE_1GB
                && (phys & (PAGE_SIZE_1GB - 1)) == 0
                && (virt & (PAGE_SIZE_1GB - 1)) == 0) {
                int32_t result = map_page_1gb(new_root, virt, phys, PAGE_KERNEL_RW);
                if (result == OK) {
                    phys += PAGE_SIZE_1GB;
                    continue;
                }
            }

            // Try 2MB page if aligned and enough space
            if (remaining >= PAGE_SIZE_2MB
                && (phys & (PAGE_SIZE_2MB - 1)) == 0
                && (virt & (PAGE_SIZE_2MB - 1)) == 0) {
                int32_t result = map_page_2mb(new_root, virt, phys, PAGE_KERNEL_RW);
                if (result == OK) {
                    phys += PAGE_SIZE_2MB;
                    continue;
                }
            }

            // Fall back to 4KB page
            int32_t result = map_page_4kb(new_root, virt, phys, PAGE_KERNEL_RW);
            if (result != OK && result != ERR_ALREADY_MAPPED) {
                log::error("paging: failed to map HHDM page at 0x%lx", virt);
            }
            phys += PAGE_SIZE_4KB;
        }
    }

    // Map kernel image in two regions:
    // 1. Unprivileged region (USER=1): __stlx_kern_start to __stlx_kern_priv_start
    //    This includes per-CPU data (.bss.percpu) which lowered threads can access.
    //    * Note: figure out how to not place per-CPU data in the privileged region.
    // 2. Privileged region (USER=0): __stlx_kern_priv_start to __stlx_kern_priv_end
    //    Only accessible at Ring 0 (elevated or kernel code).
    
    virt_addr_t kern_start = reinterpret_cast<virt_addr_t>(__stlx_kern_start);
    virt_addr_t kern_priv_start = reinterpret_cast<virt_addr_t>(__stlx_kern_priv_start);
    virt_addr_t kern_priv_end = reinterpret_cast<virt_addr_t>(__stlx_kern_priv_end);
    virt_addr_t rodata_start = reinterpret_cast<virt_addr_t>(__rodata_start);
    virt_addr_t rodata_end = reinterpret_cast<virt_addr_t>(__rodata_end);

    // Calculate physical address from virtual using boot info
    pmm::phys_addr_t kern_phys_start = kern_start - g_boot_info.kernel_virt_base + g_boot_info.kernel_phys_base;

    // Map unprivileged kernel region (USER=1) - code only, so RX (no write)
    size_t unpriv_pages = (kern_priv_start - kern_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
    for (size_t i = 0; i < unpriv_pages; i++) {
        virt_addr_t v = kern_start + i * PAGE_SIZE_4KB;
        pmm::phys_addr_t p = kern_phys_start + i * PAGE_SIZE_4KB;
        int32_t result = map_page_4kb(new_root, v, p, PAGE_USER_RX);
        if (result != OK && result != ERR_ALREADY_MAPPED) {
            log::error("paging: failed to map unprivileged kernel page at 0x%lx", v);
        }
    }

    // Map unprivileged rodata: rodata_start..__priv_rodata_start (USER_RO)
    virt_addr_t priv_rodata_start = reinterpret_cast<virt_addr_t>(__priv_rodata_start);
    if (rodata_start < priv_rodata_start) {
        pmm::phys_addr_t rodata_phys = kern_phys_start + (rodata_start - kern_start);
        size_t rodata_pages = (priv_rodata_start - rodata_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
        for (size_t i = 0; i < rodata_pages; i++) {
            virt_addr_t v = rodata_start + i * PAGE_SIZE_4KB;
            pmm::phys_addr_t p = rodata_phys + i * PAGE_SIZE_4KB;
            int32_t result = map_page_4kb(new_root, v, p, PAGE_USER_RO);
            if (result != OK && result != ERR_ALREADY_MAPPED) {
                log::error("paging: failed to map unprivileged rodata page at 0x%lx", v);
            }
        }
    }

    // Map privileged rodata: __priv_rodata_start..rodata_end (KERNEL_RO)
    if (priv_rodata_start < rodata_end) {
        pmm::phys_addr_t priv_rodata_phys = kern_phys_start + (priv_rodata_start - kern_start);
        size_t priv_rodata_pages = (rodata_end - priv_rodata_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
        for (size_t i = 0; i < priv_rodata_pages; i++) {
            virt_addr_t v = priv_rodata_start + i * PAGE_SIZE_4KB;
            pmm::phys_addr_t p = priv_rodata_phys + i * PAGE_SIZE_4KB;
            int32_t result = map_page_4kb(new_root, v, p, PAGE_KERNEL_RO);
            if (result != OK && result != ERR_ALREADY_MAPPED) {
                log::error("paging: failed to map privileged rodata page at 0x%lx", v);
            }
        }
    }

    // Map privileged code: kern_priv_start..rodata_start (KERNEL_RX)
    pmm::phys_addr_t priv_phys_start = kern_phys_start + (kern_priv_start - kern_start);
    if (kern_priv_start < rodata_start) {
        size_t priv_text_pages = (rodata_start - kern_priv_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
        for (size_t i = 0; i < priv_text_pages; i++) {
            virt_addr_t v = kern_priv_start + i * PAGE_SIZE_4KB;
            pmm::phys_addr_t p = priv_phys_start + i * PAGE_SIZE_4KB;
            int32_t result = map_page_4kb(new_root, v, p, PAGE_KERNEL_RX);
            if (result != OK && result != ERR_ALREADY_MAPPED) {
                log::error("paging: failed to map privileged text page at 0x%lx", v);
            }
        }
    }

    // Helper to map a range of 4KB pages (handles non-page-aligned boundaries)
    const auto map_range = [&](virt_addr_t start, virt_addr_t end, page_flags_t flags, const char* label) {
        virt_addr_t start_aligned = start & ~(PAGE_SIZE_4KB - 1ULL);
        virt_addr_t end_aligned = (end + PAGE_SIZE_4KB - 1) & ~(PAGE_SIZE_4KB - 1ULL);
        if (start_aligned >= end_aligned) return;
        pmm::phys_addr_t phys = kern_phys_start + (start_aligned - kern_start);
        size_t pages = (end_aligned - start_aligned) / PAGE_SIZE_4KB;
        for (size_t i = 0; i < pages; i++) {
            virt_addr_t v = start_aligned + i * PAGE_SIZE_4KB;
            pmm::phys_addr_t p = phys + i * PAGE_SIZE_4KB;
            int32_t result = map_page_4kb(new_root, v, p, flags);
            if (result != OK && result != ERR_ALREADY_MAPPED) {
                log::error("paging: failed to map %s page at 0x%lx", label, v);
            }
        }
    };

    // Map unprivileged data: rodata_end..__priv_data_start (USER_RW)
    // This includes .data, .bss.percpu, and .bss
    virt_addr_t priv_data_start = reinterpret_cast<virt_addr_t>(__priv_data_start);
    map_range(rodata_end, priv_data_start, PAGE_USER_RW, "unprivileged data");

    // Map privileged data: __priv_data_start..kern_priv_end (KERNEL_RW)
    // This includes .priv.data and .priv.bss
    map_range(priv_data_start, kern_priv_end, PAGE_KERNEL_RW, "privileged data");

    size_t total_kernel_pages = (kern_priv_end - kern_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
    log::info("paging: kernel image mapped 0x%lx-0x%lx (%zu pages, %zu KB)",
              kern_start, kern_priv_end, total_kernel_pages, total_kernel_pages * 4);

    // Switch to new page tables by writing to CR3
    set_kernel_pt_root(new_root);
    flush_tlb_all();

    g_initialized = true;

    return OK;
}

} // namespace paging
