#include "mm/paging.h"
#include "mm/paging_arch.h"
#include "mm/pmm.h"
#include "boot/boot_services.h"
#include "common/logging.h"
#include "common/string.h"
#include "hw/mmio.h"
#include "io/serial.h"
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
    extern char stlx_aarch64_vectors[];
}

namespace paging {

// Tracks whether paging has been initialized
__PRIVILEGED_DATA static bool g_initialized = false;
__PRIVILEGED_DATA static sync::spinlock g_pt_lock = sync::SPINLOCK_INIT;

// TTBR1_EL1 mask to extract physical address (mask off ASID in bits 63:48)
constexpr uint64_t TTBR_BADDR_MASK = 0x0000FFFFFFFFFFFFULL;

__PRIVILEGED_CODE pmm::phys_addr_t get_kernel_pt_root() {
    // Read TTBR1_EL1 directly - mask off ASID bits (upper 16 bits)
    return read_ttbr1_el1() & TTBR_BADDR_MASK;
}

__PRIVILEGED_CODE void set_kernel_pt_root(pmm::phys_addr_t root_pt) {
    // Ensure all page table writes are visible before switching
    // This is critical on arm64 due to weak memory ordering
    asm volatile("dsb sy" ::: "memory");
    
    write_ttbr1_el1(root_pt);
    
    // Synchronize instruction stream to use new TTBR
    asm volatile("isb" ::: "memory");
}

__PRIVILEGED_CODE void* phys_to_virt(pmm::phys_addr_t phys) {
    return reinterpret_cast<void*>(phys + g_boot_info.hhdm_offset);
}

// Allocate a zeroed page for page tables
__PRIVILEGED_CODE static pmm::phys_addr_t alloc_table_page() {
    pmm::phys_addr_t phys;
    
    if (!g_initialized) {
        // Early boot: use bootstrap allocator
        phys = pmm::bootstrap_allocator::alloc_page();
        if (phys == 0) {
            log::fatal("paging: bootstrap allocator exhausted");
        }
    } else {
        phys = pmm::alloc_page();
        if (phys == 0) {
            log::fatal("paging: out of memory for page tables");
        }
    }

    void* virt = phys_to_virt(phys);
    string::memset(virt, 0, PAGE_SIZE_4KB);
    
    return phys;
}

// Configure MAIR_EL1 with our memory attribute definitions
__PRIVILEGED_CODE static void configure_mair() {
    // Preserve index 0 so Limine's existing mappings remain valid until we switch.
    uint64_t mair = read_mair_el1();
    uint64_t original = mair;

    // Configure our indices (1-3), preserving Limine's index 0
    mair = (mair & ~(0xFFULL << (8 * mair_idx::NORMAL_WB))) |
           (mair_attr::NORMAL_WB << (8 * mair_idx::NORMAL_WB));
    mair = (mair & ~(0xFFULL << (8 * mair_idx::NORMAL_NC))) |
           (mair_attr::NORMAL_NC << (8 * mair_idx::NORMAL_NC));
    mair = (mair & ~(0xFFULL << (8 * mair_idx::DEVICE_nGnRnE))) |
           (mair_attr::DEVICE_nGnRnE << (8 * mair_idx::DEVICE_nGnRnE));

    if (mair != original) {
        write_mair_el1(mair);
        asm volatile("isb" ::: "memory");
    }
}

// Convert abstract flags to AArch64 page descriptor
__PRIVILEGED_CODE static page_desc_t flags_to_page_desc(pmm::phys_addr_t phys, page_flags_t flags) {
    page_desc_t desc = {};
    desc.valid = 1;
    desc.type = 1;  // Page descriptor (not block)
    desc.output_addr = phys >> 12;
    desc.af = 1;  // Access flag must be set

    // Shareability: Inner Shareable for SMP
    desc.sh = sh::INNER_SHAREABLE;

    // Access permissions
    if (flags & PAGE_USER) {
        if (flags & PAGE_WRITE) {
            desc.ap = ap::EL1_RW_EL0_RW;
        } else {
            desc.ap = ap::EL1_RO_EL0_RO;
        }
    } else {
        if (flags & PAGE_WRITE) {
            desc.ap = ap::EL1_RW_EL0_NONE;
        } else {
            desc.ap = ap::EL1_RO_EL0_NONE;
        }
    }

    // Execute permissions (separate for privileged and user)
    if (!(flags & PAGE_EXEC)) {
        desc.pxn = 1;  // Privileged execute never
        desc.uxn = 1;  // User execute never
    } else if (!(flags & PAGE_USER)) {
        desc.uxn = 1;  // User execute never (kernel only)
    }

    // Global flag:
    // - User mappings should be not-global (ASID-scoped)
    // - Kernel mappings remain global
    desc.ng = (flags & PAGE_USER) ? 1 : 0;

    // Memory type via MAIR index
    uint32_t mem_type = flags & PAGE_TYPE_MASK;
    if (mem_type == PAGE_DEVICE) {
        desc.attr_idx = mair_idx::DEVICE_nGnRnE;
        desc.sh = sh::OUTER_SHAREABLE;
    } else if (mem_type == PAGE_WC || mem_type == PAGE_DMA) {
        desc.attr_idx = mair_idx::NORMAL_NC;
        desc.sh = sh::INNER_SHAREABLE;
    } else {
        desc.attr_idx = mair_idx::NORMAL_WB;
        desc.sh = sh::INNER_SHAREABLE;
    }

    return desc;
}

// Convert page descriptor to abstract flags
__PRIVILEGED_CODE static page_flags_t page_desc_to_flags(const page_desc_t& desc) {
    if (!desc.valid) {
        return 0;
    }

    page_flags_t flags = PAGE_READ;

    // Access permissions
    if (desc.ap == ap::EL1_RW_EL0_RW || desc.ap == ap::EL1_RW_EL0_NONE) {
        flags |= PAGE_WRITE;
    }
    if (desc.ap == ap::EL1_RW_EL0_RW || desc.ap == ap::EL1_RO_EL0_RO) {
        flags |= PAGE_USER;
    }

    // Execute permissions
    if (!desc.pxn || !desc.uxn) {
        flags |= PAGE_EXEC;
    }

    // Global
    if (!desc.ng) {
        flags |= PAGE_GLOBAL;
    }

    // Memory type
    if (desc.attr_idx == mair_idx::DEVICE_nGnRnE) {
        flags |= PAGE_DEVICE;
    } else if (desc.attr_idx == mair_idx::NORMAL_NC) {
        flags |= PAGE_WC;
    }

    return flags;
}

// Convert abstract flags to AArch64 2MB block descriptor
__PRIVILEGED_CODE static block_desc_t flags_to_block_desc_2mb(pmm::phys_addr_t phys, page_flags_t flags) {
    block_desc_t desc = {};
    desc.valid = 1;
    desc.type = 0; // Block descriptor
    desc.output_addr = phys >> 21;
    desc.af = 1;
    desc.sh = sh::INNER_SHAREABLE;

    if (flags & PAGE_USER) {
        desc.ap = (flags & PAGE_WRITE) ? ap::EL1_RW_EL0_RW : ap::EL1_RO_EL0_RO;
    } else {
        desc.ap = (flags & PAGE_WRITE) ? ap::EL1_RW_EL0_NONE : ap::EL1_RO_EL0_NONE;
    }

    if (!(flags & PAGE_EXEC)) {
        desc.pxn = 1;
        desc.uxn = 1;
    } else if (!(flags & PAGE_USER)) {
        desc.uxn = 1;
    }

    desc.ng = (flags & PAGE_USER) ? 1 : 0;

    uint32_t mem_type = flags & PAGE_TYPE_MASK;
    if (mem_type == PAGE_DEVICE) {
        desc.attr_idx = mair_idx::DEVICE_nGnRnE;
        desc.sh = sh::OUTER_SHAREABLE;
    } else if (mem_type == PAGE_WC || mem_type == PAGE_DMA) {
        desc.attr_idx = mair_idx::NORMAL_NC;
    } else {
        desc.attr_idx = mair_idx::NORMAL_WB;
    }

    return desc;
}

// Convert 2MB block descriptor to abstract flags
__PRIVILEGED_CODE static page_flags_t block_desc_to_flags_2mb(const block_desc_t& desc) {
    if (!desc.valid) {
        return 0;
    }

    page_flags_t flags = PAGE_READ | PAGE_LARGE_2MB;

    if (desc.ap == ap::EL1_RW_EL0_RW || desc.ap == ap::EL1_RW_EL0_NONE) {
        flags |= PAGE_WRITE;
    }
    if (desc.ap == ap::EL1_RW_EL0_RW || desc.ap == ap::EL1_RO_EL0_RO) {
        flags |= PAGE_USER;
    }
    if (!desc.pxn || !desc.uxn) {
        flags |= PAGE_EXEC;
    }
    if (!desc.ng) {
        flags |= PAGE_GLOBAL;
    }
    if (desc.attr_idx == mair_idx::DEVICE_nGnRnE) {
        flags |= PAGE_DEVICE;
    } else if (desc.attr_idx == mair_idx::NORMAL_NC) {
        flags |= PAGE_WC;
    }

    return flags;
}

// Convert abstract flags to AArch64 1GB block descriptor
__PRIVILEGED_CODE static block_desc_t flags_to_block_desc_1gb(pmm::phys_addr_t phys, page_flags_t flags) {
    block_desc_t desc = {};
    desc.valid = 1;
    desc.type = 0; // Block descriptor
    desc.output_addr = phys >> 21; // 1GB-aligned phys ensures bits [29:21] are 0
    desc.af = 1;
    desc.sh = sh::INNER_SHAREABLE;

    if (flags & PAGE_USER) {
        desc.ap = (flags & PAGE_WRITE) ? ap::EL1_RW_EL0_RW : ap::EL1_RO_EL0_RO;
    } else {
        desc.ap = (flags & PAGE_WRITE) ? ap::EL1_RW_EL0_NONE : ap::EL1_RO_EL0_NONE;
    }

    if (!(flags & PAGE_EXEC)) {
        desc.pxn = 1;
        desc.uxn = 1;
    } else if (!(flags & PAGE_USER)) {
        desc.uxn = 1;
    }

    desc.ng = (flags & PAGE_USER) ? 1 : 0;

    uint32_t mem_type = flags & PAGE_TYPE_MASK;
    if (mem_type == PAGE_DEVICE) {
        desc.attr_idx = mair_idx::DEVICE_nGnRnE;
        desc.sh = sh::OUTER_SHAREABLE;
    } else if (mem_type == PAGE_WC || mem_type == PAGE_DMA) {
        desc.attr_idx = mair_idx::NORMAL_NC;
    } else {
        desc.attr_idx = mair_idx::NORMAL_WB;
    }

    return desc;
}

// Convert 1GB block descriptor to abstract flags
__PRIVILEGED_CODE static page_flags_t block_desc_to_flags_1gb(const block_desc_t& desc) {
    if (!desc.valid) {
        return 0;
    }

    page_flags_t flags = PAGE_READ | PAGE_HUGE_1GB;

    if (desc.ap == ap::EL1_RW_EL0_RW || desc.ap == ap::EL1_RW_EL0_NONE) {
        flags |= PAGE_WRITE;
    }
    if (desc.ap == ap::EL1_RW_EL0_RW || desc.ap == ap::EL1_RO_EL0_RO) {
        flags |= PAGE_USER;
    }
    if (!desc.pxn || !desc.uxn) {
        flags |= PAGE_EXEC;
    }
    if (!desc.ng) {
        flags |= PAGE_GLOBAL;
    }
    if (desc.attr_idx == mair_idx::DEVICE_nGnRnE) {
        flags |= PAGE_DEVICE;
    } else if (desc.attr_idx == mair_idx::NORMAL_NC) {
        flags |= PAGE_WC;
    }

    return flags;
}

// Get or create translation table at specified level
// Returns virtual address of table, creates if needed
__PRIVILEGED_CODE static translation_table_t* get_or_create_table(table_desc_t* entry) {
    if (entry->valid && entry->type == 1) {
        return static_cast<translation_table_t*>(phys_to_virt(entry->next_table_addr << 12));
    }

    // Allocate new table
    pmm::phys_addr_t table_phys = alloc_table_page();

    // Set up table descriptor
    entry->value = 0;
    entry->valid = 1;
    entry->type = 1;  // Table descriptor
    entry->next_table_addr = table_phys >> 12;

    return static_cast<translation_table_t*>(phys_to_virt(table_phys));
}

// Get pointer to page descriptor for a virtual address, creating tables as needed if create=true
// Returns nullptr if not mapped and create=false
__PRIVILEGED_CODE static page_desc_t* get_page_desc_ptr(pmm::phys_addr_t root_pt, virt_addr_t virt, bool create) {
    auto parts = split_virt_addr(virt);
    translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));

    // L0 -> L1
    table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];
    if (!l0_entry->valid && !create) return nullptr;
    translation_table_t* l1 = create ? get_or_create_table(l0_entry) :
        static_cast<translation_table_t*>(phys_to_virt(l0_entry->next_table_addr << 12));
    if (!l1) return nullptr;

    // L1 -> L2
    // Check if it's a 1GB block
    if (l1->as_block[parts.l1_idx].valid && l1->as_block[parts.l1_idx].type == 0) {
        return nullptr;  // 1GB block, can't get page descriptor
    }
    table_desc_t* l1_entry = &l1->as_table[parts.l1_idx];
    if (!l1_entry->valid && !create) return nullptr;
    translation_table_t* l2 = create ? get_or_create_table(l1_entry) :
        static_cast<translation_table_t*>(phys_to_virt(l1_entry->next_table_addr << 12));
    if (!l2) return nullptr;

    // L2 -> L3
    // Check if it's a 2MB block
    if (l2->as_block[parts.l2_idx].valid && l2->as_block[parts.l2_idx].type == 0) {
        return nullptr;  // 2MB block, can't get page descriptor
    }
    table_desc_t* l2_entry = &l2->as_table[parts.l2_idx];
    if (!l2_entry->valid && !create) return nullptr;
    translation_table_t* l3 = create ? get_or_create_table(l2_entry) :
        static_cast<translation_table_t*>(phys_to_virt(l2_entry->next_table_addr << 12));
    if (!l3) return nullptr;

    return &l3->as_page[parts.l3_idx];
}

// Map a single 4KB page
__PRIVILEGED_CODE static int32_t map_page_4kb(pmm::phys_addr_t root_pt, virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags) {
    page_desc_t* desc = get_page_desc_ptr(root_pt, virt, true);
    if (!desc) {
        return ERR_INVALID_ADDR;
    }

    if (desc->valid) {
        return ERR_ALREADY_MAPPED;
    }

    *desc = flags_to_page_desc(phys, flags);
    return OK;
}

// Map a single 2MB block
__PRIVILEGED_CODE static int32_t map_block_2mb(pmm::phys_addr_t root_pt, virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags) {
    auto parts = split_virt_addr(virt);
    translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));

    // L0 -> L1
    table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];
    translation_table_t* l1 = get_or_create_table(l0_entry);

    // Check for 1GB block conflict at L1
    if (l1->as_block[parts.l1_idx].valid && l1->as_block[parts.l1_idx].type == 0) {
        return ERR_ALREADY_MAPPED;
    }

    // L1 -> L2
    table_desc_t* l1_entry = &l1->as_table[parts.l1_idx];
    translation_table_t* l2 = get_or_create_table(l1_entry);

    // Set L2 entry as 2MB block (no L3 level)
    block_desc_t* block = &l2->as_block[parts.l2_idx];
    if (block->valid) {
        // L2 entry exists — if it's a table descriptor (type==1) pointing
        // to an L3, check if all 512 L3 entries are empty. If so, free
        // the L3 table and reclaim the entry.
        table_desc_t* as_tbl = &l2->as_table[parts.l2_idx];
        if (as_tbl->type == 1) {
            auto* l3 = static_cast<translation_table_t*>(
                phys_to_virt(static_cast<pmm::phys_addr_t>(as_tbl->next_table_addr) << 12));
            bool all_empty = true;
            for (int i = 0; i < 512; i++) {
                if (l3->raw[i] & 1) { // valid bit
                    all_empty = false;
                    break;
                }
            }
            if (!all_empty) {
                return ERR_ALREADY_MAPPED;
            }
            pmm::phys_addr_t l3_phys = static_cast<pmm::phys_addr_t>(as_tbl->next_table_addr) << 12;
            l2->raw[parts.l2_idx] = 0;
            pmm::free_page(l3_phys);
            block = &l2->as_block[parts.l2_idx];
        } else {
            return ERR_ALREADY_MAPPED;
        }
    }

    *block = flags_to_block_desc_2mb(phys, flags);
    return OK;
}

// Map a single 1GB block
__PRIVILEGED_CODE static int32_t map_block_1gb(pmm::phys_addr_t root_pt, virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags) {
    auto parts = split_virt_addr(virt);
    translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));

    // L0 -> L1
    table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];
    translation_table_t* l1 = get_or_create_table(l0_entry);

    // Set L1 entry as 1GB block (no L2 or L3 levels)
    block_desc_t* block = &l1->as_block[parts.l1_idx];
    if (block->valid) {
        table_desc_t* as_tbl = &l1->as_table[parts.l1_idx];
        if (as_tbl->type == 1) {
            auto* l2 = static_cast<translation_table_t*>(
                phys_to_virt(static_cast<pmm::phys_addr_t>(as_tbl->next_table_addr) << 12));
            bool all_empty = true;
            for (int i = 0; i < 512; i++) {
                if (l2->raw[i] & 1) {
                    all_empty = false;
                    break;
                }
            }
            if (!all_empty) {
                return ERR_ALREADY_MAPPED;
            }
            pmm::phys_addr_t l2_phys = static_cast<pmm::phys_addr_t>(as_tbl->next_table_addr) << 12;
            l1->raw[parts.l1_idx] = 0;
            pmm::free_page(l2_phys);
            block = &l1->as_block[parts.l1_idx];
        } else {
            return ERR_ALREADY_MAPPED;
        }
    }

    *block = flags_to_block_desc_1gb(phys, flags);
    return OK;
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
        page_desc_t* desc = get_page_desc_ptr(root_pt, v, false);
        if (desc && desc->valid) {
            return ERR_ALREADY_MAPPED;
        }
    }

    // Second pass: map all pages
    for (size_t i = 0; i < count; i++) {
        virt_addr_t v = virt + i * PAGE_SIZE_4KB;
        pmm::phys_addr_t p = phys + i * PAGE_SIZE_4KB;
        int32_t result = map_page_4kb(root_pt, v, p, flags);
        if (result != OK) {
            return result;
        }
    }

    return OK;
}

// Check if all 512 entries in a translation table are empty.
__PRIVILEGED_CODE static bool is_table_empty(const translation_table_t* table) {
    for (int i = 0; i < 512; i++) {
        if (table->raw[i] != 0) return false;
    }
    return true;
}

// Free a page table page back to PMM if it was dynamically allocated.
// Bootstrap-allocated pages (PAGE_FLAG_RESERVED) are not freed.
__PRIVILEGED_CODE static void try_free_table_page(pmm::phys_addr_t phys) {
    auto* pfd = pmm::get_page_frame(phys);
    if (pfd && pfd->is_allocated()) {
        // A reclaimed translation-table page may be reused quickly for unrelated
        // allocations. Before releasing it to PMM, invalidate all EL1 TLB state
        // to ensure no CPU retains stale table-walk references to this page.
        flush_tlb_all();
        pmm::free_page(phys);
    }
}

__PRIVILEGED_CODE static int32_t unmap_page_nolock(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return OK;
    }

    auto parts = split_virt_addr(virt);
    translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));

    table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];
    if (!l0_entry->valid) return OK;

    translation_table_t* l1 = static_cast<translation_table_t*>(
        phys_to_virt(l0_entry->next_table_addr << 12));

    // 1GB block at L1
    if (l1->as_block[parts.l1_idx].valid && l1->as_block[parts.l1_idx].type == 0) {
        l1->raw[parts.l1_idx] = 0;
        flush_tlb_page(virt);
        if (is_table_empty(l1)) {
            pmm::phys_addr_t l1_phys = static_cast<pmm::phys_addr_t>(l0_entry->next_table_addr) << 12;
            l0_entry->value = 0;
            try_free_table_page(l1_phys);
        }
        return OK;
    }

    table_desc_t* l1_entry = &l1->as_table[parts.l1_idx];
    if (!l1_entry->valid) return OK;

    translation_table_t* l2 = static_cast<translation_table_t*>(
        phys_to_virt(l1_entry->next_table_addr << 12));

    // 2MB block at L2
    if (l2->as_block[parts.l2_idx].valid && l2->as_block[parts.l2_idx].type == 0) {
        l2->raw[parts.l2_idx] = 0;
        flush_tlb_page(virt);
        if (is_table_empty(l2)) {
            pmm::phys_addr_t l2_phys = static_cast<pmm::phys_addr_t>(l1_entry->next_table_addr) << 12;
            l1_entry->value = 0;
            try_free_table_page(l2_phys);
            if (is_table_empty(l1)) {
                pmm::phys_addr_t l1_phys = static_cast<pmm::phys_addr_t>(l0_entry->next_table_addr) << 12;
                l0_entry->value = 0;
                try_free_table_page(l1_phys);
            }
        }
        return OK;
    }

    table_desc_t* l2_entry = &l2->as_table[parts.l2_idx];
    if (!l2_entry->valid) return OK;

    translation_table_t* l3 = static_cast<translation_table_t*>(
        phys_to_virt(l2_entry->next_table_addr << 12));

    // 4KB page at L3
    page_desc_t* page = &l3->as_page[parts.l3_idx];
    if (!page->valid) return OK;

    page->value = 0;
    flush_tlb_page(virt);

    // Cascade: reclaim empty page tables up the hierarchy
    if (is_table_empty(l3)) {
        pmm::phys_addr_t l3_phys = static_cast<pmm::phys_addr_t>(l2_entry->next_table_addr) << 12;
        l2_entry->value = 0;
        try_free_table_page(l3_phys);
        if (is_table_empty(l2)) {
            pmm::phys_addr_t l2_phys = static_cast<pmm::phys_addr_t>(l1_entry->next_table_addr) << 12;
            l1_entry->value = 0;
            try_free_table_page(l2_phys);
            if (is_table_empty(l1)) {
                pmm::phys_addr_t l1_phys = static_cast<pmm::phys_addr_t>(l0_entry->next_table_addr) << 12;
                l0_entry->value = 0;
                try_free_table_page(l1_phys);
            }
        }
    }

    return OK;
}

__PRIVILEGED_CODE int32_t unmap_page(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    sync::irq_lock_guard guard(g_pt_lock);
    return unmap_page_nolock(virt, root_pt);
}

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
        return map_block_1gb(root_pt, virt, phys, flags);
    }

    if (flags & PAGE_LARGE_2MB) {
        if ((virt & (PAGE_SIZE_2MB - 1)) != 0 || (phys & (PAGE_SIZE_2MB - 1)) != 0) {
            return ERR_ALIGNMENT;
        }
        return map_block_2mb(root_pt, virt, phys, flags);
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
    translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));

    table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];
    if (!l0_entry->valid) return ERR_NOT_MAPPED;

    translation_table_t* l1 = static_cast<translation_table_t*>(
        phys_to_virt(l0_entry->next_table_addr << 12));

    // 1GB block
    if (l1->as_block[parts.l1_idx].valid && l1->as_block[parts.l1_idx].type == 0) {
        block_desc_t* block = &l1->as_block[parts.l1_idx];
        pmm::phys_addr_t phys = static_cast<pmm::phys_addr_t>(block->output_addr) << 21;
        *block = flags_to_block_desc_1gb(phys, flags);
        flush_tlb_page(virt);
        return OK;
    }

    table_desc_t* l1_entry = &l1->as_table[parts.l1_idx];
    if (!l1_entry->valid) return ERR_NOT_MAPPED;

    translation_table_t* l2 = static_cast<translation_table_t*>(
        phys_to_virt(l1_entry->next_table_addr << 12));

    // 2MB block
    if (l2->as_block[parts.l2_idx].valid && l2->as_block[parts.l2_idx].type == 0) {
        block_desc_t* block = &l2->as_block[parts.l2_idx];
        pmm::phys_addr_t phys = static_cast<pmm::phys_addr_t>(block->output_addr) << 21;
        *block = flags_to_block_desc_2mb(phys, flags);
        flush_tlb_page(virt);
        return OK;
    }

    table_desc_t* l2_entry = &l2->as_table[parts.l2_idx];
    if (!l2_entry->valid) return ERR_NOT_MAPPED;

    translation_table_t* l3 = static_cast<translation_table_t*>(
        phys_to_virt(l2_entry->next_table_addr << 12));

    // 4KB page
    page_desc_t* page = &l3->as_page[parts.l3_idx];
    if (!page->valid) return ERR_NOT_MAPPED;

    pmm::phys_addr_t phys = static_cast<pmm::phys_addr_t>(page->output_addr) << 12;
    *page = flags_to_page_desc(phys, flags);
    flush_tlb_page(virt);
    return OK;
}

__PRIVILEGED_CODE pmm::phys_addr_t get_physical(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return 0;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    auto parts = split_virt_addr(virt);
    translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));

    table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];
    if (!l0_entry->valid) return 0;

    translation_table_t* l1 = static_cast<translation_table_t*>(
        phys_to_virt(l0_entry->next_table_addr << 12));

    // 1GB block
    if (l1->as_block[parts.l1_idx].valid && l1->as_block[parts.l1_idx].type == 0) {
        pmm::phys_addr_t base = static_cast<pmm::phys_addr_t>(l1->as_block[parts.l1_idx].output_addr) << 21;
        return base + (virt & (PAGE_SIZE_1GB - 1));
    }

    table_desc_t* l1_entry = &l1->as_table[parts.l1_idx];
    if (!l1_entry->valid) return 0;

    translation_table_t* l2 = static_cast<translation_table_t*>(
        phys_to_virt(l1_entry->next_table_addr << 12));

    // 2MB block
    if (l2->as_block[parts.l2_idx].valid && l2->as_block[parts.l2_idx].type == 0) {
        pmm::phys_addr_t base = static_cast<pmm::phys_addr_t>(l2->as_block[parts.l2_idx].output_addr) << 21;
        return base + (virt & (PAGE_SIZE_2MB - 1));
    }

    table_desc_t* l2_entry = &l2->as_table[parts.l2_idx];
    if (!l2_entry->valid) return 0;

    translation_table_t* l3 = static_cast<translation_table_t*>(
        phys_to_virt(l2_entry->next_table_addr << 12));

    page_desc_t* page = &l3->as_page[parts.l3_idx];
    if (!page->valid) return 0;

    pmm::phys_addr_t base = static_cast<pmm::phys_addr_t>(page->output_addr) << 12;
    return base + (virt & (PAGE_SIZE_4KB - 1));
}

__PRIVILEGED_CODE page_flags_t get_page_flags(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return 0;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    auto parts = split_virt_addr(virt);
    translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));

    table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];
    if (!l0_entry->valid) return 0;

    translation_table_t* l1 = static_cast<translation_table_t*>(
        phys_to_virt(l0_entry->next_table_addr << 12));

    // 1GB block
    if (l1->as_block[parts.l1_idx].valid && l1->as_block[parts.l1_idx].type == 0) {
        return block_desc_to_flags_1gb(l1->as_block[parts.l1_idx]);
    }

    table_desc_t* l1_entry = &l1->as_table[parts.l1_idx];
    if (!l1_entry->valid) return 0;

    translation_table_t* l2 = static_cast<translation_table_t*>(
        phys_to_virt(l1_entry->next_table_addr << 12));

    // 2MB block
    if (l2->as_block[parts.l2_idx].valid && l2->as_block[parts.l2_idx].type == 0) {
        return block_desc_to_flags_2mb(l2->as_block[parts.l2_idx]);
    }

    table_desc_t* l2_entry = &l2->as_table[parts.l2_idx];
    if (!l2_entry->valid) return 0;

    translation_table_t* l3 = static_cast<translation_table_t*>(
        phys_to_virt(l2_entry->next_table_addr << 12));

    page_desc_t* page = &l3->as_page[parts.l3_idx];
    if (!page->valid) return 0;

    return page_desc_to_flags(*page);
}

__PRIVILEGED_CODE bool is_mapped(virt_addr_t virt, pmm::phys_addr_t root_pt) {
    if (!g_initialized) {
        return false;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    auto parts = split_virt_addr(virt);
    translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));

    table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];
    if (!l0_entry->valid) return false;

    translation_table_t* l1 = static_cast<translation_table_t*>(
        phys_to_virt(l0_entry->next_table_addr << 12));

    // 1GB block
    if (l1->as_block[parts.l1_idx].valid && l1->as_block[parts.l1_idx].type == 0) {
        return true;
    }

    table_desc_t* l1_entry = &l1->as_table[parts.l1_idx];
    if (!l1_entry->valid) return false;

    translation_table_t* l2 = static_cast<translation_table_t*>(
        phys_to_virt(l1_entry->next_table_addr << 12));

    // 2MB block
    if (l2->as_block[parts.l2_idx].valid && l2->as_block[parts.l2_idx].type == 0) {
        return true;
    }

    table_desc_t* l2_entry = &l2->as_table[parts.l2_idx];
    if (!l2_entry->valid) return false;

    translation_table_t* l3 = static_cast<translation_table_t*>(
        phys_to_virt(l2_entry->next_table_addr << 12));

    return l3->as_page[parts.l3_idx].valid;
}

__PRIVILEGED_CODE void flush_tlb_page(virt_addr_t virt) {
    tlbi_vae1is(virt);
}

__PRIVILEGED_CODE void flush_tlb_range(virt_addr_t start, virt_addr_t end) {
    sync::irq_lock_guard guard(g_pt_lock);

    pmm::phys_addr_t root_pt = get_kernel_pt_root();
    virt_addr_t addr = start;

    while (addr < end) {
        size_t step = PAGE_SIZE_4KB;

        auto parts = split_virt_addr(addr);
        translation_table_t* l0 = static_cast<translation_table_t*>(phys_to_virt(root_pt));
        table_desc_t* l0_entry = &l0->as_table[parts.l0_idx];

        if (l0_entry->valid) {
            translation_table_t* l1 = static_cast<translation_table_t*>(
                phys_to_virt(l0_entry->next_table_addr << 12));

            if (l1->as_block[parts.l1_idx].valid && l1->as_block[parts.l1_idx].type == 0) {
                step = PAGE_SIZE_1GB;
            } else if (l1->as_table[parts.l1_idx].valid) {
                translation_table_t* l2 = static_cast<translation_table_t*>(
                    phys_to_virt(l1->as_table[parts.l1_idx].next_table_addr << 12));

                if (l2->as_block[parts.l2_idx].valid && l2->as_block[parts.l2_idx].type == 0) {
                    step = PAGE_SIZE_2MB;
                }
            }
        }

        tlbi_vae1is(addr);
        addr += step;
    }
}

__PRIVILEGED_CODE void flush_tlb_all() {
    // Full TLB invalidation broadcast to all CPUs (inner shareable domain).
    // Pattern from Linux's flush_tlb_all().
    asm volatile(
        "dsb ishst\n"       // Ensure prior page table stores are visible
        "tlbi vmalle1is\n"  // Invalidate all EL1 TLB entries on all CPUs
        "dsb ish\n"         // Ensure invalidation completes
        "isb"               // Synchronize instruction stream
        ::: "memory"
    );
}

__PRIVILEGED_CODE void dump_mappings() {
    if (!g_initialized) {
        log::info("paging: not initialized");
        return;
    }

    sync::irq_lock_guard guard(g_pt_lock);

    pmm::phys_addr_t root_pt = get_kernel_pt_root();
    log::info("paging: root_pt=0x%lx", root_pt);
    
    virt_addr_t l0_virt = reinterpret_cast<virt_addr_t>(phys_to_virt(root_pt));
    
    translation_table_t* l0 = reinterpret_cast<translation_table_t*>(l0_virt);

    uint64_t mapped_pages = 0;
    uint64_t mapped_2mb = 0;
    uint64_t mapped_1gb = 0;

    for (int l0_idx = 0; l0_idx < 512; l0_idx++) {
        table_desc_t* l0_entry = &l0->as_table[l0_idx];
        if (!l0_entry->valid) continue;

        translation_table_t* l1 = static_cast<translation_table_t*>(
            phys_to_virt(l0_entry->next_table_addr << 12));

        for (int l1_idx = 0; l1_idx < 512; l1_idx++) {
            if (!l1->as_table[l1_idx].valid) continue;

            // Check for 1GB block
            if (l1->as_block[l1_idx].type == 0) {
                mapped_1gb++;
                continue;
            }

            translation_table_t* l2 = static_cast<translation_table_t*>(
                phys_to_virt(l1->as_table[l1_idx].next_table_addr << 12));

            for (int l2_idx = 0; l2_idx < 512; l2_idx++) {
                if (!l2->as_table[l2_idx].valid) continue;

                // Check for 2MB block
                if (l2->as_block[l2_idx].type == 0) {
                    mapped_2mb++;
                    continue;
                }

                translation_table_t* l3 = static_cast<translation_table_t*>(
                    phys_to_virt(l2->as_table[l2_idx].next_table_addr << 12));

                for (int l3_idx = 0; l3_idx < 512; l3_idx++) {
                    if (l3->as_page[l3_idx].valid) {
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

    configure_mair();

    pmm::phys_addr_t new_root = alloc_table_page();

    // Map HHDM region for all usable memory, using block mappings where aligned
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

            // Try 1GB block if aligned and enough space
            if (remaining >= PAGE_SIZE_1GB
                && (phys & (PAGE_SIZE_1GB - 1)) == 0
                && (virt & (PAGE_SIZE_1GB - 1)) == 0) {
                int32_t result = map_block_1gb(new_root, virt, phys, PAGE_KERNEL_RW);
                if (result == OK) {
                    phys += PAGE_SIZE_1GB;
                    continue;
                }
            }

            // Try 2MB block if aligned and enough space
            if (remaining >= PAGE_SIZE_2MB
                && (phys & (PAGE_SIZE_2MB - 1)) == 0
                && (virt & (PAGE_SIZE_2MB - 1)) == 0) {
                int32_t result = map_block_2mb(new_root, virt, phys, PAGE_KERNEL_RW);
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

    // Map kernel image regions
    virt_addr_t kern_start = reinterpret_cast<virt_addr_t>(__stlx_kern_start);
    virt_addr_t kern_priv_start = reinterpret_cast<virt_addr_t>(__stlx_kern_priv_start);
    virt_addr_t kern_priv_end = reinterpret_cast<virt_addr_t>(__stlx_kern_priv_end);
    virt_addr_t rodata_start = reinterpret_cast<virt_addr_t>(__rodata_start);
    virt_addr_t rodata_end = reinterpret_cast<virt_addr_t>(__rodata_end);
    pmm::phys_addr_t kern_phys_start = kern_start - g_boot_info.kernel_virt_base + g_boot_info.kernel_phys_base;

    // Unprivileged code (USER_RX)
    size_t unpriv_pages = (kern_priv_start - kern_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
    for (size_t i = 0; i < unpriv_pages; i++) {
        map_page_4kb(new_root, kern_start + i * PAGE_SIZE_4KB,
                    kern_phys_start + i * PAGE_SIZE_4KB, PAGE_USER_RX);
    }

    // Unprivileged rodata: rodata_start..__priv_rodata_start (USER_RO)
    virt_addr_t priv_rodata_start = reinterpret_cast<virt_addr_t>(__priv_rodata_start);
    if (rodata_start < priv_rodata_start) {
        pmm::phys_addr_t rodata_phys = kern_phys_start + (rodata_start - kern_start);
        size_t rodata_pages = (priv_rodata_start - rodata_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
        for (size_t i = 0; i < rodata_pages; i++) {
            map_page_4kb(new_root, rodata_start + i * PAGE_SIZE_4KB,
                        rodata_phys + i * PAGE_SIZE_4KB, PAGE_USER_RO);
        }
    }

    // Privileged rodata: __priv_rodata_start..rodata_end (KERNEL_RO)
    if (priv_rodata_start < rodata_end) {
        pmm::phys_addr_t priv_rodata_phys = kern_phys_start + (priv_rodata_start - kern_start);
        size_t priv_rodata_pages = (rodata_end - priv_rodata_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
        for (size_t i = 0; i < priv_rodata_pages; i++) {
            map_page_4kb(new_root, priv_rodata_start + i * PAGE_SIZE_4KB,
                        priv_rodata_phys + i * PAGE_SIZE_4KB, PAGE_KERNEL_RO);
        }
    }

    // Privileged code (KERNEL_RX)
    pmm::phys_addr_t priv_phys_start = kern_phys_start + (kern_priv_start - kern_start);
    if (kern_priv_start < rodata_start) {
        size_t priv_text_pages = (rodata_start - kern_priv_start + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
        for (size_t i = 0; i < priv_text_pages; i++) {
            map_page_4kb(new_root, kern_priv_start + i * PAGE_SIZE_4KB,
                        priv_phys_start + i * PAGE_SIZE_4KB, PAGE_KERNEL_RX);
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

    // Clear SCTLR RES0 bit 20 if set (can cause spurious faults on some cores)
    uint64_t sctlr = read_sctlr_el1();
    constexpr uint64_t SCTLR_RES0_BIT20 = 1ULL << 20;
    if (sctlr & SCTLR_RES0_BIT20) {
        sctlr &= ~SCTLR_RES0_BIT20;
        write_sctlr_el1(sctlr);
    }

    // Switch to new page tables
    set_kernel_pt_root(new_root);
    flush_tlb_all();

    g_initialized = true;
    log::info("paging: page tables active");

    return OK;
}

__PRIVILEGED_CODE pmm::phys_addr_t create_user_pt_root() {
    pmm::phys_addr_t new_root = pmm::alloc_page();
    if (new_root == 0) {
        return 0;
    }

    // aarch64 uses split translation roots:
    // - TTBR1_EL1: kernel mappings
    // - TTBR0_EL1: user mappings
    //
    // User roots must therefore start empty and contain only user-space
    // mappings. Copying kernel L0 entries into TTBR0 would alias kernel
    // table hierarchy into user roots and can introduce EL0 permission
    // restrictions through table-level AP limits.
    auto* new_l0 = static_cast<uint64_t*>(phys_to_virt(new_root));
    string::memset(new_l0, 0, PAGE_SIZE_4KB);

    return new_root;
}

__PRIVILEGED_CODE void destroy_user_pt_root(pmm::phys_addr_t root) {
    if (root != 0) {
        pmm::free_page(root);
    }
}

__PRIVILEGED_CODE pmm::phys_addr_t supervisor_pt_root_for_user_task(pmm::phys_addr_t) {
    return get_kernel_pt_root(); // aarch64: TTBR1 stays kernel, TTBR0 is set per-task via user_pt_root
}

} // namespace paging
