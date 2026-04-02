#include "drivers/graphics/nvidia/nv_gsp_boot.h"
#include "drivers/graphics/nvidia/nv_gpu.h"
#include "drivers/graphics/nvidia/nv_fwsec.h"
#include "hw/delay.h"
#include "clock/clock.h"
#include "dma/dma.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
#include "mm/pmm_types.h"

namespace nv {

// ============================================================================
// Helper: Align down
// ============================================================================

static uint64_t align_down(uint64_t val, uint64_t alignment) {
    return val & ~(alignment - 1);
}

static uint64_t align_up(uint64_t val, uint64_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

static uint64_t div_round_up(uint64_t num, uint64_t denom) {
    return (num + denom - 1) / denom;
}

// ============================================================================
// LibOS ID8 Encoding
// ============================================================================

uint64_t libos_id8(const char* name) {
    uint64_t id = 0;
    for (int i = 0; i < 8 && name[i] != '\0'; i++) {
        id = (id << 8) | static_cast<uint8_t>(name[i]);
    }
    return id;
}

// ============================================================================
// 1. Framebuffer Layout Computation
// ============================================================================

int32_t gsp_compute_fb_layout(nv_gpu* gpu, const gsp_firmware& fw, fb_layout& layout) {
    log::info("nvidia: gsp: computing framebuffer layout");

    // Read VRAM size (GA102+: register 0x1183a4, value << 20)
    uint32_t vidmem_reg = gpu->reg_rd32(reg::VIDMEM_SIZE_GA102);
    layout.fb_size = static_cast<uint64_t>(vidmem_reg) << 20;

    if (layout.fb_size == 0) {
        log::error("nvidia: gsp: VRAM size is 0");
        return ERR_INVALID;
    }

    log::info("nvidia: gsp: VRAM size: %lu MB (%lu bytes)", layout.fb_size >> 20, layout.fb_size);

    // VGA workspace: at top of FB
    // Check NV_PDISP_VGA_WORKSPACE_BASE (0x625f04)
    uint32_t vga_reg = gpu->reg_rd32(reg::VGA_WORKSPACE_BASE);
    uint64_t vga_base = layout.fb_size - 0x100000; // Default: 1MB from end

    if (vga_reg & 0x08) { // status_valid bit
        uint64_t vga_addr = (static_cast<uint64_t>(vga_reg & 0xFFFFFF00)) << 8;
        if (vga_addr > 0 && vga_addr >= vga_base) {
            vga_base = vga_addr;
        } else if (vga_addr > 0 && vga_addr < vga_base) {
            vga_base = layout.fb_size - 0x20000; // 128KB from end
        }
    }

    layout.vga_workspace.addr = vga_base;
    layout.vga_workspace.size = layout.fb_size - vga_base;
    layout.bios.addr = vga_base;
    layout.bios.size = layout.vga_workspace.size;

    log::info("nvidia: gsp: VGA workspace: 0x%lx (%lu KB)", vga_base, layout.vga_workspace.size >> 10);

    // FRTS region: 1MB, below 128KB-aligned VGA workspace
    layout.frts.size = 0x100000; // 1MB
    layout.frts.addr = align_down(vga_base, ALIGN_128K) - layout.frts.size;

    log::info("nvidia: gsp: FRTS: 0x%lx - 0x%lx (%lu KB)",
              layout.frts.addr, layout.frts.addr + layout.frts.size, layout.frts.size >> 10);

    // Boot firmware: bootloader ucode size, 4KB-aligned below FRTS
    layout.boot.size = fw.bootloader.payload_size;
    layout.boot.addr = align_down(layout.frts.addr - layout.boot.size, ALIGN_4K);

    log::info("nvidia: gsp: Boot: 0x%lx - 0x%lx (%lu bytes)",
              layout.boot.addr, layout.boot.addr + layout.boot.size, layout.boot.size);

    // GSP FW ELF: .fwimage size, 64KB-aligned below boot
    layout.elf.size = fw.gsp.fwimage_size;
    layout.elf.addr = align_down(layout.boot.addr - layout.elf.size, ALIGN_64K);

    log::info("nvidia: gsp: ELF: 0x%lx - 0x%lx (%lu MB)",
              layout.elf.addr, layout.elf.addr + layout.elf.size, layout.elf.size >> 20);

    // GSP heap: computed from FB size
    // GA102+ (chipset >= 0x172) uses LIBOS3, Turing/GA100 uses LIBOS2
    bool is_libos3 = (gpu->chipset() >= 0x172);
    uint32_t os_carveout = is_libos3 ? GSP_FW_HEAP_OS_CARVEOUT_LIBOS3 : GSP_FW_HEAP_OS_CARVEOUT_LIBOS2;
    uint32_t heap_min = is_libos3 ? GSP_FW_HEAP_MIN_LIBOS3 : GSP_FW_HEAP_MIN_LIBOS2;
    uint32_t heap_max = is_libos3 ? GSP_FW_HEAP_MAX_LIBOS3 : GSP_FW_HEAP_MAX_LIBOS2;

    uint64_t fb_size_gb = div_round_up(layout.fb_size, 1ULL << 30);
    uint64_t heap_size = os_carveout +
                         GSP_FW_HEAP_BASE_SIZE +
                         align_up(static_cast<uint64_t>(GSP_FW_HEAP_PER_GB_SIZE) * fb_size_gb, ALIGN_1M) +
                         align_up(GSP_FW_HEAP_CLIENT_ALLOC, ALIGN_1M);

    // Clamp to [min, max]
    if (heap_size < heap_min) heap_size = heap_min;
    if (heap_size > heap_max) heap_size = heap_max;

    log::info("nvidia: gsp: heap sizing: libos%s carveout=%uMB base=8MB per_gb=%luMB client=96MB → %luMB (clamped [%u,%u]MB)",
              is_libos3 ? "3" : "2", os_carveout >> 20, 
              align_up(static_cast<uint64_t>(GSP_FW_HEAP_PER_GB_SIZE) * fb_size_gb, ALIGN_1M) >> 20,
              heap_size >> 20, heap_min >> 20, heap_max >> 20);

    layout.heap.addr = align_down(layout.elf.addr - heap_size, ALIGN_1M);
    layout.heap.size = align_down(layout.elf.addr - layout.heap.addr, ALIGN_1M);

    log::info("nvidia: gsp: Heap: 0x%lx - 0x%lx (%lu MB, fb_gb=%lu)",
              layout.heap.addr, layout.heap.addr + layout.heap.size,
              layout.heap.size >> 20, fb_size_gb);

    // WPR2 region: encompasses heap+elf+boot+frts
    // Start: 1MB-aligned below heap (leaving room for GspFwWprMeta)
    layout.wpr2.addr = align_down(layout.heap.addr - 256, ALIGN_1M); // 256 = sizeof(gsp_fw_wpr_meta)
    layout.wpr2.size = (layout.frts.addr + layout.frts.size) - layout.wpr2.addr;

    log::info("nvidia: gsp: WPR2: 0x%lx - 0x%lx (%lu MB)",
              layout.wpr2.addr, layout.wpr2.addr + layout.wpr2.size, layout.wpr2.size >> 20);

    // Non-WPR heap: 1MB below WPR2 start
    layout.non_wpr_heap.size = 0x100000; // 1MB
    layout.non_wpr_heap.addr = layout.wpr2.addr - layout.non_wpr_heap.size;

    log::info("nvidia: gsp: Non-WPR heap: 0x%lx (%lu MB)", layout.non_wpr_heap.addr, layout.non_wpr_heap.size >> 20);

    return OK;
}

// ============================================================================
// 2. Radix-3 Page Table Construction
// ============================================================================

int32_t gsp_build_radix3(nv_gpu* /*gpu*/, const gsp_fw& fw_data, radix3& r3) {
    log::info("nvidia: gsp: building radix-3 page table for %u byte firmware", fw_data.fwimage_size);

    r3.valid = false;
    string::memset(r3.lvl, 0, sizeof(r3.lvl));

    // The firmware image needs to be in DMA-accessible pages.
    // We allocate individual pages, copy the firmware data, and build page tables.
    uint32_t fw_pages = div_round_up(fw_data.fwimage_size, GSP_PAGE_SIZE_VAL);
    uint32_t lvl2_entries = fw_pages;
    uint32_t lvl2_pages = div_round_up(lvl2_entries * sizeof(uint64_t), GSP_PAGE_SIZE_VAL);
    uint32_t lvl1_entries = lvl2_pages;
    uint32_t lvl1_pages = div_round_up(lvl1_entries * sizeof(uint64_t), GSP_PAGE_SIZE_VAL);
    // Level 0: always 1 page

    log::info("nvidia: gsp: radix3: fw_pages=%u, lvl2=%u pages, lvl1=%u pages, lvl0=1 page",
              fw_pages, lvl2_pages, lvl1_pages);

    int32_t rc;

    // Allocate level 2 (leaf) — needs to hold fw_pages entries
    uint32_t lvl2_size = align_up(lvl2_entries * sizeof(uint64_t), GSP_PAGE_SIZE_VAL);
    RUN_ELEVATED(rc = dma::alloc_pages(lvl2_size / pmm::PAGE_SIZE, r3.lvl[2], pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: gsp: radix3 level 2 alloc failed (%u bytes): %d", lvl2_size, rc);
        return ERR_NOT_FOUND;
    }

    // Allocate level 1 (middle) — needs lvl2_pages entries
    uint32_t lvl1_size = align_up(lvl1_entries * sizeof(uint64_t), GSP_PAGE_SIZE_VAL);
    RUN_ELEVATED(rc = dma::alloc_pages(lvl1_size / pmm::PAGE_SIZE, r3.lvl[1], pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: gsp: radix3 level 1 alloc failed: %d", rc);
        RUN_ELEVATED(dma::free_pages(r3.lvl[2]));
        return ERR_NOT_FOUND;
    }

    // Allocate level 0 (root) — 1 page, 1 entry
    RUN_ELEVATED(rc = dma::alloc_pages(1, r3.lvl[0], pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: gsp: radix3 level 0 alloc failed: %d", rc);
        RUN_ELEVATED(dma::free_pages(r3.lvl[1]));
        RUN_ELEVATED(dma::free_pages(r3.lvl[2]));
        return ERR_NOT_FOUND;
    }

    // Now we need to allocate DMA pages for the actual firmware data.
    // Since Stellux's DMA allocator gives us contiguous physical memory,
    // we allocate the firmware as one large DMA buffer and fill level 2
    // entries pointing to individual pages within it.
    dma::buffer fw_dma = {};
    uint32_t fw_dma_pages = div_round_up(fw_data.fwimage_size, pmm::PAGE_SIZE);
    RUN_ELEVATED(rc = dma::alloc_pages(fw_dma_pages, fw_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: gsp: firmware DMA alloc failed (%u pages): %d", fw_dma_pages, rc);
        RUN_ELEVATED(dma::free_pages(r3.lvl[0]));
        RUN_ELEVATED(dma::free_pages(r3.lvl[1]));
        RUN_ELEVATED(dma::free_pages(r3.lvl[2]));
        return ERR_NOT_FOUND;
    }

    // Copy firmware image to DMA buffer
    string::memcpy(reinterpret_cast<void*>(fw_dma.virt), fw_data.fwimage, fw_data.fwimage_size);
    // Zero remainder
    if (fw_dma.size > fw_data.fwimage_size) {
        string::memset(reinterpret_cast<void*>(fw_dma.virt + fw_data.fwimage_size), 0,
                       fw_dma.size - fw_data.fwimage_size);
    }

    log::info("nvidia: gsp: firmware copied to DMA: phys=0x%lx size=0x%lx", fw_dma.phys, fw_dma.size);

    // Fill level 2 entries: each points to a 4KB firmware page
    volatile uint64_t* l2 = reinterpret_cast<volatile uint64_t*>(r3.lvl[2].virt);
    for (uint32_t i = 0; i < fw_pages; i++) {
        l2[i] = fw_dma.phys + i * GSP_PAGE_SIZE_VAL;
    }

    // Fill level 1 entries: each points to a level 2 page
    volatile uint64_t* l1 = reinterpret_cast<volatile uint64_t*>(r3.lvl[1].virt);
    for (uint32_t i = 0; i < lvl2_pages; i++) {
        l1[i] = r3.lvl[2].phys + i * GSP_PAGE_SIZE_VAL;
    }

    // Fill level 0 entry: points to level 1 page
    volatile uint64_t* l0 = reinterpret_cast<volatile uint64_t*>(r3.lvl[0].virt);
    l0[0] = r3.lvl[1].phys;

    // Store firmware DMA buffer in radix3 struct (must persist while GSP runs)
    r3.fw_data_dma = fw_dma;
    r3.valid = true;

    log::info("nvidia: gsp: radix3 built: lvl0=0x%lx → lvl1=0x%lx → lvl2=0x%lx → fw=0x%lx",
              r3.lvl[0].phys, r3.lvl[1].phys, r3.lvl[2].phys, r3.fw_data_dma.phys);

    return OK;
}

// ============================================================================
// 3. WPR Metadata Initialization
// ============================================================================

int32_t gsp_init_wpr_meta(nv_gpu* /*gpu*/, const gsp_firmware& /*fw*/,
                           const fb_layout& layout, gsp_boot_state& state) {
    log::info("nvidia: gsp: initializing WPR metadata");

    // Allocate 4KB DMA page for WPR meta
    int32_t rc;
    RUN_ELEVATED(rc = dma::alloc_pages(1, state.wpr_meta_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: gsp: WPR meta alloc failed: %d", rc);
        return ERR_NOT_FOUND;
    }

    gsp_fw_wpr_meta* meta = reinterpret_cast<gsp_fw_wpr_meta*>(state.wpr_meta_dma.virt);
    string::memset(meta, 0, sizeof(gsp_fw_wpr_meta));

    meta->magic    = GSP_FW_WPR_META_MAGIC;
    meta->revision = GSP_FW_WPR_META_REVISION;

    // Radix3 page table root
    meta->sysmem_addr_radix3_elf = state.r3.lvl[0].phys;
    meta->size_of_radix3_elf     = layout.elf.size; // = .fwimage size

    // Bootloader
    meta->sysmem_addr_bootloader = state.bootloader_dma.phys;
    meta->size_of_bootloader     = state.bootloader_dma.size;
    meta->bootloader_code_offset = state.boot_code_offset;
    meta->bootloader_data_offset = state.boot_data_offset;
    meta->bootloader_manifest_offset = state.boot_manifest_offset;

    // Signatures
    meta->sysmem_addr_signature = state.sig_dma.phys;
    meta->size_of_signature     = state.sig_dma.size;

    // FB layout
    meta->gsp_fw_rsvd_start   = layout.non_wpr_heap.addr;
    meta->non_wpr_heap_offset = layout.non_wpr_heap.addr;
    meta->non_wpr_heap_size   = layout.non_wpr_heap.size;
    meta->gsp_fw_wpr_start    = layout.wpr2.addr;
    meta->gsp_fw_heap_offset  = layout.heap.addr;
    meta->gsp_fw_heap_size    = layout.heap.size;
    meta->gsp_fw_offset       = layout.elf.addr;
    meta->boot_bin_offset     = layout.boot.addr;
    meta->frts_offset         = layout.frts.addr;
    meta->frts_size           = layout.frts.size;
    meta->gsp_fw_wpr_end      = align_down(layout.vga_workspace.addr, ALIGN_128K);
    meta->fb_size             = layout.fb_size;
    meta->vga_workspace_offset = layout.vga_workspace.addr;
    meta->vga_workspace_size   = layout.vga_workspace.size;
    meta->boot_count          = 0;
    meta->verified            = 0; // Set by booter

    log::info("nvidia: gsp: WPR meta at phys=0x%lx:", state.wpr_meta_dma.phys);
    log::info("nvidia: gsp:   radix3=0x%lx fw_size=0x%lx",
              meta->sysmem_addr_radix3_elf, meta->size_of_radix3_elf);
    log::info("nvidia: gsp:   bootloader=0x%lx size=0x%lx code=0x%lx data=0x%lx manifest=0x%lx",
              meta->sysmem_addr_bootloader, meta->size_of_bootloader,
              meta->bootloader_code_offset, meta->bootloader_data_offset,
              meta->bootloader_manifest_offset);
    log::info("nvidia: gsp:   sigs=0x%lx size=0x%lx", meta->sysmem_addr_signature, meta->size_of_signature);
    log::info("nvidia: gsp:   wpr2=[0x%lx, 0x%lx) heap=0x%lx elf=0x%lx boot=0x%lx frts=0x%lx",
              meta->gsp_fw_wpr_start, meta->gsp_fw_wpr_end,
              meta->gsp_fw_heap_offset, meta->gsp_fw_offset,
              meta->boot_bin_offset, meta->frts_offset);

    return OK;
}

// ============================================================================
// 4. Shared Memory + Message Queue Initialization
// ============================================================================

int32_t gsp_init_shared_mem(nv_gpu* /*gpu*/, gsp_boot_state& state) {
    log::info("nvidia: gsp: initializing shared memory and message queues");

    // Calculate PTE count
    // PTEs map the queue pages so GSP can DMA-access them
    uint32_t queue_pages = (CMDQ_SIZE + MSGQ_SIZE) / GSP_PAGE_SIZE_VAL; // 128
    uint32_t pte_entries = queue_pages;
    // Add pages for the PTE array itself
    pte_entries += div_round_up(pte_entries * sizeof(uint64_t), GSP_PAGE_SIZE_VAL);
    state.pte_count = pte_entries;
    state.pte_size = align_up(pte_entries * sizeof(uint64_t), GSP_PAGE_SIZE_VAL);

    // Total shared memory: PTE array + cmdq + msgq
    uint32_t shm_total = state.pte_size + CMDQ_SIZE + MSGQ_SIZE;
    uint32_t shm_pages = shm_total / GSP_PAGE_SIZE_VAL;

    state.cmdq_offset = state.pte_size;
    state.msgq_offset = state.pte_size + CMDQ_SIZE;

    log::info("nvidia: gsp: shared memory: ptes=%u (%u bytes), cmdq=0x%x, msgq=0x%x, total=%u",
              state.pte_count, state.pte_size, state.cmdq_offset, state.msgq_offset, shm_total);

    // Allocate shared memory (must be contiguous for DMA)
    int32_t rc;
    RUN_ELEVATED(rc = dma::alloc_pages(shm_pages, state.shm_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: gsp: shared memory alloc failed (%u pages): %d", shm_pages, rc);
        return ERR_NOT_FOUND;
    }

    log::info("nvidia: gsp: shared memory: phys=0x%lx virt=0x%lx size=%u",
              state.shm_dma.phys, state.shm_dma.virt, shm_total);

    // Zero everything
    string::memset(reinterpret_cast<void*>(state.shm_dma.virt), 0, shm_total);

    // Fill PTE array: self-referencing page addresses
    volatile uint64_t* ptes = reinterpret_cast<volatile uint64_t*>(state.shm_dma.virt);
    for (uint32_t i = 0; i < state.pte_count; i++) {
        ptes[i] = state.shm_dma.phys + i * GSP_PAGE_SIZE_VAL;
    }

    // Initialize command queue header
    queue_header* cmdq_hdr = reinterpret_cast<queue_header*>(state.shm_dma.virt + state.cmdq_offset);
    cmdq_hdr->tx.version   = 0;
    cmdq_hdr->tx.size      = CMDQ_SIZE;
    cmdq_hdr->tx.msg_size  = QUEUE_ENTRY_SIZE;
    cmdq_hdr->tx.msg_count = (CMDQ_SIZE - QUEUE_ENTRY_SIZE) / QUEUE_ENTRY_SIZE; // 63
    cmdq_hdr->tx.write_ptr = 0;
    cmdq_hdr->tx.flags     = 1; // "I want to swap RX"
    cmdq_hdr->tx.rx_hdr_off = sizeof(msgq_tx_header); // 32 (offset of rx within queue_header)
    cmdq_hdr->tx.entry_off  = QUEUE_ENTRY_SIZE; // 0x1000
    cmdq_hdr->rx.read_ptr   = 0;

    // Initialize message/status queue header
    queue_header* msgq_hdr = reinterpret_cast<queue_header*>(state.shm_dma.virt + state.msgq_offset);
    msgq_hdr->tx.version   = 0;
    msgq_hdr->tx.size      = MSGQ_SIZE;
    msgq_hdr->tx.msg_size  = QUEUE_ENTRY_SIZE;
    msgq_hdr->tx.msg_count = (MSGQ_SIZE - QUEUE_ENTRY_SIZE) / QUEUE_ENTRY_SIZE; // 63
    msgq_hdr->tx.write_ptr = 0;
    msgq_hdr->tx.flags     = 0;
    msgq_hdr->tx.rx_hdr_off = sizeof(msgq_tx_header); // 32
    msgq_hdr->tx.entry_off  = QUEUE_ENTRY_SIZE;
    msgq_hdr->rx.read_ptr   = 0;

    state.cmdq_seq = 0;
    state.rpc_seq = 0;

    log::info("nvidia: gsp: queues initialized: cmdq_count=%u msgq_count=%u",
              cmdq_hdr->tx.msg_count, msgq_hdr->tx.msg_count);

    return OK;
}

// ============================================================================
// 5. LibOS Arguments Initialization
// ============================================================================

static void create_pte_array(volatile uint64_t* ptes, uint64_t dma_addr, uint32_t size) {
    uint32_t num_pages = div_round_up(size, GSP_PAGE_SIZE_VAL);
    for (uint32_t i = 0; i < num_pages; i++) {
        ptes[i] = dma_addr + i * GSP_PAGE_SIZE_VAL;
    }
}

int32_t gsp_init_libos(nv_gpu* /*gpu*/, gsp_boot_state& state) {
    log::info("nvidia: gsp: initializing LibOS arguments");

    int32_t rc;

    // Allocate libos page (4KB, holds 4 × 32-byte entries)
    RUN_ELEVATED(rc = dma::alloc_pages(1, state.libos_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) { log::error("nvidia: gsp: libos alloc failed"); return ERR_NOT_FOUND; }

    // Allocate log buffers (64KB each)
    RUN_ELEVATED(rc = dma::alloc_pages(LOG_BUF_SIZE / pmm::PAGE_SIZE, state.loginit_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) { log::error("nvidia: gsp: loginit alloc failed"); return ERR_NOT_FOUND; }

    RUN_ELEVATED(rc = dma::alloc_pages(LOG_BUF_SIZE / pmm::PAGE_SIZE, state.logintr_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) { log::error("nvidia: gsp: logintr alloc failed"); return ERR_NOT_FOUND; }

    RUN_ELEVATED(rc = dma::alloc_pages(LOG_BUF_SIZE / pmm::PAGE_SIZE, state.logrm_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) { log::error("nvidia: gsp: logrm alloc failed"); return ERR_NOT_FOUND; }

    // Allocate rmargs (4KB)
    RUN_ELEVATED(rc = dma::alloc_pages(1, state.rmargs_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) { log::error("nvidia: gsp: rmargs alloc failed"); return ERR_NOT_FOUND; }

    // Zero all buffers
    string::memset(reinterpret_cast<void*>(state.loginit_dma.virt), 0, LOG_BUF_SIZE);
    string::memset(reinterpret_cast<void*>(state.logintr_dma.virt), 0, LOG_BUF_SIZE);
    string::memset(reinterpret_cast<void*>(state.logrm_dma.virt), 0, LOG_BUF_SIZE);
    string::memset(reinterpret_cast<void*>(state.rmargs_dma.virt), 0, GSP_PAGE_SIZE_VAL);
    string::memset(reinterpret_cast<void*>(state.libos_dma.virt), 0, GSP_PAGE_SIZE_VAL);

    // Fill PTE arrays in log buffers (at offset 8, after the put_pointer u64)
    create_pte_array(reinterpret_cast<volatile uint64_t*>(state.loginit_dma.virt + 8),
                     state.loginit_dma.phys, LOG_BUF_SIZE);
    create_pte_array(reinterpret_cast<volatile uint64_t*>(state.logintr_dma.virt + 8),
                     state.logintr_dma.phys, LOG_BUF_SIZE);
    create_pte_array(reinterpret_cast<volatile uint64_t*>(state.logrm_dma.virt + 8),
                     state.logrm_dma.phys, LOG_BUF_SIZE);

    // Populate RM arguments (GSP_ARGUMENTS_CACHED)
    gsp_arguments_cached* rmargs = reinterpret_cast<gsp_arguments_cached*>(state.rmargs_dma.virt);
    string::memset(rmargs, 0, sizeof(gsp_arguments_cached));

    rmargs->mq_init.shared_mem_phys_addr = state.shm_dma.phys;
    rmargs->mq_init.page_table_entry_count = state.pte_count;
    rmargs->mq_init._pad0 = 0;
    rmargs->mq_init.cmd_queue_offset = static_cast<uint64_t>(state.cmdq_offset);
    rmargs->mq_init.stat_queue_offset = static_cast<uint64_t>(state.msgq_offset);
    rmargs->mq_init.lockless_cmd_queue_offset = 0;
    rmargs->mq_init.lockless_stat_queue_offset = 0;
    rmargs->sr_init.old_level = 0;
    rmargs->sr_init.flags = 0;
    rmargs->sr_init.b_in_pm_transition = 0;

    log::info("nvidia: gsp: rmargs: shm=0x%lx pte_count=%u cmdq_off=0x%x msgq_off=0x%x",
              rmargs->mq_init.shared_mem_phys_addr, rmargs->mq_init.page_table_entry_count,
              rmargs->mq_init.cmd_queue_offset, rmargs->mq_init.stat_queue_offset);

    // Populate LibOS argument array (4 entries)
    libos_mem_region_init_arg* args =
        reinterpret_cast<libos_mem_region_init_arg*>(state.libos_dma.virt);

    // Entry 0: LOGINIT
    args[0].id8  = libos_id8("LOGINIT");
    args[0].pa   = state.loginit_dma.phys;
    args[0].size = LOG_BUF_SIZE;
    args[0].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    args[0].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;

    // Entry 1: LOGINTR
    args[1].id8  = libos_id8("LOGINTR");
    args[1].pa   = state.logintr_dma.phys;
    args[1].size = LOG_BUF_SIZE;
    args[1].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    args[1].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;

    // Entry 2: LOGRM
    args[2].id8  = libos_id8("LOGRM");
    args[2].pa   = state.logrm_dma.phys;
    args[2].size = LOG_BUF_SIZE;
    args[2].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    args[2].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;

    // Entry 3: RMARGS
    args[3].id8  = libos_id8("RMARGS");
    args[3].pa   = state.rmargs_dma.phys;
    args[3].size = GSP_PAGE_SIZE_VAL;
    args[3].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    args[3].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;

    log::info("nvidia: gsp: libos array at phys=0x%lx:", state.libos_dma.phys);
    for (int i = 0; i < 4; i++) {
        log::info("nvidia: gsp:   [%d] id=0x%lx pa=0x%lx size=0x%lx kind=%u loc=%u",
                  i, args[i].id8, args[i].pa, args[i].size, args[i].kind, args[i].loc);
    }

    return OK;
}

// ============================================================================
// 6. Run SEC2 Booter
// ============================================================================

int32_t gsp_run_booter(nv_gpu* gpu, gsp_firmware& fw, gsp_boot_state& state) {
    log::info("nvidia: gsp: ========================================");
    log::info("nvidia: gsp: Running SEC2 booter to load GSP-RM");
    log::info("nvidia: gsp: ========================================");

    int32_t rc;

    // Initialize SEC2 falcon
    falcon sec2;
    rc = sec2.init(gpu, falcon::engine_type::SEC2);
    if (rc != OK) return rc;

    // Allocate DMA buffer for booter_load payload
    booter_fw& booter = fw.booter_load;
    uint32_t payload_pages = div_round_up(booter.payload_size, pmm::PAGE_SIZE);
    dma::buffer booter_dma = {};
    RUN_ELEVATED(rc = dma::alloc_pages(payload_pages, booter_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: gsp: booter DMA alloc failed: %d", rc);
        return ERR_NOT_FOUND;
    }

    // Copy booter payload to DMA buffer
    string::memcpy(reinterpret_cast<void*>(booter_dma.virt), booter.payload, booter.payload_size);

    // Select and patch signature for the booter
    // For SEC2: fuse register at 0x824140 + (ucode_id-1)*4
    uint32_t fuse_base = (booter.sig_meta.engine_id_mask & 0x0001) ?
        reg::FUSE_SEC2_BASE : reg::FUSE_GSP_BASE;
    uint32_t fuse_addr = fuse_base + (booter.sig_meta.ucode_id - 1) * 4;
    uint32_t fuse_val = gpu->reg_rd32(fuse_addr);

    log::info("nvidia: gsp: booter fuse: addr=0x%06x val=0x%08x (engine=0x%x ucode=%u)",
              fuse_addr, fuse_val, booter.sig_meta.engine_id_mask, booter.sig_meta.ucode_id);

    // Booter signature selection uses OpenRM's reverse-index scheme:
    //   fuseVer = fls(fuse_val)  (1-based highest set bit position)
    //   sigIndex = numSigs - 1 - fuseVer
    // Reference: OpenRM s_patchBooterUcodeSignature in kernel_gsp_fwsec.c
    // This is DIFFERENT from FWSEC's popcount-based selection.
    uint32_t fuse_ver = 0;
    {
        uint32_t tmp = fuse_val;
        while (tmp) { fuse_ver++; tmp >>= 1; }
    }

    uint32_t num_sigs = booter.num_sig_val;
    if (num_sigs == 0) {
        log::error("nvidia: gsp: booter has 0 signatures");
        RUN_ELEVATED(dma::free_pages(booter_dma));
        return ERR_INVALID;
    }

    uint32_t idx = 0;
    if (num_sigs > 1) {
        if (fuse_ver > num_sigs - 1) {
            log::error("nvidia: gsp: booter fuse_ver %u exceeds num_sigs %u", fuse_ver, num_sigs);
            RUN_ELEVATED(dma::free_pages(booter_dma));
            return ERR_INVALID;
        }
        idx = num_sigs - 1 - fuse_ver;
    }

    log::info("nvidia: gsp: booter sig: fuse_ver=%u num_sigs=%u → idx=%u",
              fuse_ver, num_sigs, idx);

    // dmem_sign_offset: offset within DMEM where the signature lives (for BROM_PARAADDR)
    // patch_loc_val: absolute offset in the payload where the signature must be patched
    uint32_t dmem_sign_offset = booter.patch_loc_val - booter.load_hdr.os_data_offset;
    uint32_t sig_patch_offset = booter.patch_loc_val;
    uint32_t sig_offset = booter.hs_hdr.sig_prod_offset + booter.patch_sig_val +
                          idx * booter.per_sig_size;

    if (sig_offset + booter.per_sig_size <= booter.raw_size &&
        sig_patch_offset + booter.per_sig_size <= booter.payload_size) {
        string::memcpy(reinterpret_cast<void*>(booter_dma.virt + sig_patch_offset),
                       booter.raw_data + sig_offset, booter.per_sig_size);
        log::info("nvidia: gsp: booter sig patched: %u bytes at payload offset 0x%x (DMEM offset 0x%x)",
                  booter.per_sig_size, sig_patch_offset, dmem_sign_offset);
    } else {
        log::error("nvidia: gsp: booter sig patch out of bounds");
        RUN_ELEVATED(dma::free_pages(booter_dma));
        return ERR_INVALID;
    }

    // Reset SEC2 falcon
    rc = sec2.reset();
    if (rc != OK) {
        RUN_ELEVATED(dma::free_pages(booter_dma));
        return rc;
    }

    // DMA load IMEM + DMEM onto SEC2
    rc = sec2.dma_load(booter_dma,
                       0, booter.app.offset, booter.app.size,           // IMEM
                       0, booter.load_hdr.os_data_offset, booter.load_hdr.os_data_size, // DMEM
                       true); // secure IMEM
    if (rc != OK) {
        log::error("nvidia: gsp: SEC2 DMA load failed: %d", rc);
        RUN_ELEVATED(dma::free_pages(booter_dma));
        return rc;
    }

    // Program BROM for SEC2
    rc = sec2.program_brom(dmem_sign_offset,
                           booter.sig_meta.engine_id_mask,
                           booter.sig_meta.ucode_id);
    if (rc != OK) {
        RUN_ELEVATED(dma::free_pages(booter_dma));
        return rc;
    }

    // Boot SEC2 with WPR meta address in mailboxes
    uint32_t mbox0 = static_cast<uint32_t>(state.wpr_meta_dma.phys & 0xFFFFFFFF);
    uint32_t mbox1 = static_cast<uint32_t>(state.wpr_meta_dma.phys >> 32);
    uint32_t out_mbox0 = 0, out_mbox1 = 0;

    log::info("nvidia: gsp: booting SEC2 with wpr_meta=0x%lx (mbox0=0x%x mbox1=0x%x)",
              state.wpr_meta_dma.phys, mbox0, mbox1);

    rc = sec2.boot_and_wait(booter.boot_addr, mbox0, mbox1, out_mbox0, out_mbox1);
    if (rc != OK) {
        log::error("nvidia: gsp: SEC2 boot failed: %d", rc);
        RUN_ELEVATED(dma::free_pages(booter_dma));
        return rc;
    }

    // Check result
    if (out_mbox0 != 0) {
        log::error("nvidia: gsp: SEC2 booter failed: mbox0=0x%08x", out_mbox0);
        RUN_ELEVATED(dma::free_pages(booter_dma));
        return ERR_IO;
    }

    log::info("nvidia: gsp: SEC2 booter completed successfully");

    // Write app_version to GSP falcon RM register
    falcon gsp_flcn;
    gsp_flcn.init(gpu, falcon::engine_type::GSP);
    gsp_flcn.wr32(0x080, state.boot_app_version);

    // Verify GSP RISC-V is active
    uint32_t riscv_status = gsp_flcn.rd32(FALCON_RISCV_STATUS);
    if (!(riscv_status & FALCON_RISCV_ACTIVE)) {
        log::error("nvidia: gsp: GSP RISC-V not active after boot (status=0x%08x)", riscv_status);
        RUN_ELEVATED(dma::free_pages(booter_dma));
        return ERR_IO;
    }

    log::info("nvidia: gsp: GSP RISC-V active (status=0x%08x)", riscv_status);

    // Free booter DMA (no longer needed)
    RUN_ELEVATED(dma::free_pages(booter_dma));

    return OK;
}

// ============================================================================
// 7. Wait for INIT_DONE
// ============================================================================

int32_t gsp_wait_init_done(nv_gpu* /*gpu*/, gsp_boot_state& state) {
    log::info("nvidia: gsp: waiting for GSP INIT_DONE (timeout 4s)...");

    // The status/message queue is at state.shm_dma.virt + state.msgq_offset
    // GSP writes messages here. We poll for a message with function == 0x1001.

    volatile queue_header* msgq_hdr = reinterpret_cast<volatile queue_header*>(
        state.shm_dma.virt + state.msgq_offset);

    // The cmdq RX header (where our read pointer for the msgq lives)
    // is at the cmdq header's rx field. But per the cross-queue pointer layout:
    // msgq.rptr = cmdq.rx.read_ptr
    volatile uint32_t* msgq_rptr = &reinterpret_cast<volatile queue_header*>(
        state.shm_dma.virt + state.cmdq_offset)->rx.read_ptr;

    uint64_t deadline = clock::now_ns() + 4000000000ULL; // 4 seconds
    uint32_t msg_count = msgq_hdr->tx.msg_count;
    uint32_t entry_off = msgq_hdr->tx.entry_off;

    while (clock::now_ns() < deadline) {
        // Check if there's a message: wptr != rptr
        uint32_t wptr = msgq_hdr->tx.write_ptr;
        uint32_t rptr = *msgq_rptr;

        if (wptr == rptr) {
            delay::us(1000); // 1ms poll interval
            continue;
        }

        // Read message at rptr
        uint8_t* entry = reinterpret_cast<uint8_t*>(state.shm_dma.virt +
                         state.msgq_offset + entry_off + rptr * QUEUE_ENTRY_SIZE);

        // Parse message element header
        gsp_msg_element* msg = reinterpret_cast<gsp_msg_element*>(entry);
        rpc_header* rpc = reinterpret_cast<rpc_header*>(entry + sizeof(gsp_msg_element));

        log::info("nvidia: gsp: received message: func=0x%04x result=0x%x seq=%u",
                  rpc->function, rpc->rpc_result, rpc->sequence);

        // Advance read pointer
        uint32_t new_rptr = (rptr + msg->elem_count) % msg_count;
        *msgq_rptr = new_rptr;

        // Check for INIT_DONE
        if (rpc->function == NV_VGPU_MSG_EVENT_GSP_INIT_DONE) {
            if (rpc->rpc_result != 0) {
                log::error("nvidia: gsp: INIT_DONE with error: result=0x%x", rpc->rpc_result);
                return ERR_IO;
            }
            log::info("nvidia: gsp: ========================================");
            log::info("nvidia: gsp: GSP INIT_DONE received! GSP-RM is running.");
            log::info("nvidia: gsp: ========================================");
            return OK;
        }

        // Handle sequencer events
        if (rpc->function == NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQ) {
            log::info("nvidia: gsp: received CPU sequencer request (processing...)");
            // TODO: Process sequencer opcodes
            // For initial boot, these are typically suspend/resume related
            // and may not appear during first boot
        }

        // Handle error events
        if (rpc->function == NV_VGPU_MSG_EVENT_OS_ERROR_LOG) {
            log::warn("nvidia: gsp: received OS error log from GSP");
        }
    }

    log::error("nvidia: gsp: INIT_DONE timeout (4 seconds)");
    return ERR_TIMEOUT;
}

// ============================================================================
// Master GSP Boot Function
// ============================================================================

int32_t gsp_boot(nv_gpu* gpu, gsp_firmware& fw, gsp_boot_state& state) {
    log::info("nvidia: ========================================");
    log::info("nvidia: Phase C: GSP Boot Sequence");
    log::info("nvidia: ========================================");

    string::memset(&state, 0, sizeof(state));
    state.initialized = false;

    int32_t rc;

    // Step 1: Compute FB layout
    rc = gsp_compute_fb_layout(gpu, fw, state.layout);
    if (rc != OK) {
        log::error("nvidia: gsp: FB layout computation failed: %d", rc);
        return rc;
    }

    // Step 2: Build radix-3 page table for GSP firmware
    rc = gsp_build_radix3(gpu, fw.gsp, state.r3);
    if (rc != OK) {
        log::error("nvidia: gsp: radix-3 build failed: %d", rc);
        return rc;
    }

    // Step 3: Allocate DMA for bootloader ucode
    uint32_t bl_pages = div_round_up(fw.bootloader.payload_size, pmm::PAGE_SIZE);
    RUN_ELEVATED(rc = dma::alloc_pages(bl_pages, state.bootloader_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: gsp: bootloader DMA alloc failed: %d", rc);
        return ERR_NOT_FOUND;
    }
    string::memcpy(reinterpret_cast<void*>(state.bootloader_dma.virt),
                   fw.bootloader.payload, fw.bootloader.payload_size);

    state.boot_code_offset = fw.bootloader.code_offset;
    state.boot_data_offset = fw.bootloader.data_offset;
    state.boot_manifest_offset = fw.bootloader.manifest_offset;
    state.boot_app_version = fw.bootloader.app_version;

    log::info("nvidia: gsp: bootloader DMA: phys=0x%lx size=%u",
              state.bootloader_dma.phys, fw.bootloader.payload_size);

    // Step 4: Allocate DMA for GSP firmware signatures
    if (fw.gsp.fwsig && fw.gsp.fwsig_size > 0) {
        uint32_t sig_size = align_up(fw.gsp.fwsig_size, DMEM_ALIGN);
        uint32_t sig_pages = div_round_up(sig_size, pmm::PAGE_SIZE);
        RUN_ELEVATED(rc = dma::alloc_pages(sig_pages, state.sig_dma, pmm::ZONE_DMA32, paging::PAGE_USER));
        if (rc != dma::OK) {
            log::error("nvidia: gsp: signature DMA alloc failed: %d", rc);
            return ERR_NOT_FOUND;
        }
        string::memcpy(reinterpret_cast<void*>(state.sig_dma.virt),
                       fw.gsp.fwsig, fw.gsp.fwsig_size);
        log::info("nvidia: gsp: signatures DMA: phys=0x%lx size=%u", state.sig_dma.phys, fw.gsp.fwsig_size);
    }

    // Step 5: Run FWSEC-FRTS (establish WPR2)
    falcon gsp_flcn;
    gsp_flcn.init(gpu, falcon::engine_type::GSP);
    rc = fwsec_run_frts(gpu, fw.fwsec, gsp_flcn);
    if (rc != OK) {
        log::error("nvidia: gsp: FWSEC-FRTS failed: %d", rc);
        return rc;
    }

    // Step 6: Initialize shared memory and message queues
    rc = gsp_init_shared_mem(gpu, state);
    if (rc != OK) {
        log::error("nvidia: gsp: shared mem init failed: %d", rc);
        return rc;
    }

    // Step 7: Initialize LibOS arguments
    rc = gsp_init_libos(gpu, state);
    if (rc != OK) {
        log::error("nvidia: gsp: libos init failed: %d", rc);
        return rc;
    }

    // Step 8: Initialize WPR metadata
    rc = gsp_init_wpr_meta(gpu, fw, state.layout, state);
    if (rc != OK) {
        log::error("nvidia: gsp: WPR meta init failed: %d", rc);
        return rc;
    }

    // Step 9: Write pre-boot RPCs to command queue
    // GSP-RM reads these from the command queue during initialization.
    // SET_SYSTEM_INFO (fn=72) and SET_REGISTRY (fn=73) must be present.
    {
        // Write SET_SYSTEM_INFO RPC to command queue
        uint8_t* cmdq_base = reinterpret_cast<uint8_t*>(state.shm_dma.virt + state.cmdq_offset);
        queue_header* cmdq_hdr_ptr = reinterpret_cast<queue_header*>(cmdq_base);
        uint32_t entry_off_val = cmdq_hdr_ptr->tx.entry_off;

        // RPC 1: SET_SYSTEM_INFO (function 72)
        {
            uint8_t* entry = cmdq_base + entry_off_val + state.cmdq_seq * QUEUE_ENTRY_SIZE;
            string::memset(entry, 0, QUEUE_ENTRY_SIZE);

            gsp_msg_element* msg = reinterpret_cast<gsp_msg_element*>(entry);
            rpc_header* rpc = reinterpret_cast<rpc_header*>(entry + sizeof(gsp_msg_element));

            msg->elem_count = 1;
            msg->seq_num = state.cmdq_seq;
            rpc->header_version = RPC_HDR_VERSION;
            rpc->signature = RPC_SIGNATURE;
            rpc->length = sizeof(rpc_header); // Header-only, minimal payload
            rpc->function = NV_VGPU_MSG_FUNCTION_SET_SYSTEM_INFO;
            rpc->rpc_result = 0xFFFFFFFF;
            rpc->rpc_result_private = 0xFFFFFFFF;
            rpc->sequence = state.rpc_seq++;

            // Compute XOR checksum
            msg->pad = 0;
            msg->checksum = 0;
            uint64_t csum = 0;
            for (uint32_t i = 0; i < QUEUE_ENTRY_SIZE / sizeof(uint64_t); i++) {
                csum ^= reinterpret_cast<volatile uint64_t*>(entry)[i];
            }
            msg->checksum = static_cast<uint32_t>((csum >> 32) ^ (csum & 0xFFFFFFFF));

            state.cmdq_seq++;
            cmdq_hdr_ptr->tx.write_ptr = state.cmdq_seq;
            log::info("nvidia: gsp: queued SET_SYSTEM_INFO (fn=%u) at cmdq slot %u",
                      NV_VGPU_MSG_FUNCTION_SET_SYSTEM_INFO, state.cmdq_seq - 1);
        }

        // RPC 2: SET_REGISTRY (function 73)
        {
            uint8_t* entry = cmdq_base + entry_off_val + state.cmdq_seq * QUEUE_ENTRY_SIZE;
            string::memset(entry, 0, QUEUE_ENTRY_SIZE);

            gsp_msg_element* msg = reinterpret_cast<gsp_msg_element*>(entry);
            rpc_header* rpc = reinterpret_cast<rpc_header*>(entry + sizeof(gsp_msg_element));

            msg->elem_count = 1;
            msg->seq_num = state.cmdq_seq;
            rpc->header_version = RPC_HDR_VERSION;
            rpc->signature = RPC_SIGNATURE;
            rpc->length = sizeof(rpc_header);
            rpc->function = NV_VGPU_MSG_FUNCTION_SET_REGISTRY;
            rpc->rpc_result = 0xFFFFFFFF;
            rpc->rpc_result_private = 0xFFFFFFFF;
            rpc->sequence = state.rpc_seq++;

            msg->pad = 0;
            msg->checksum = 0;
            uint64_t csum = 0;
            for (uint32_t i = 0; i < QUEUE_ENTRY_SIZE / sizeof(uint64_t); i++) {
                csum ^= reinterpret_cast<volatile uint64_t*>(entry)[i];
            }
            msg->checksum = static_cast<uint32_t>((csum >> 32) ^ (csum & 0xFFFFFFFF));

            state.cmdq_seq++;
            cmdq_hdr_ptr->tx.write_ptr = state.cmdq_seq;
            log::info("nvidia: gsp: queued SET_REGISTRY (fn=%u) at cmdq slot %u",
                      NV_VGPU_MSG_FUNCTION_SET_REGISTRY, state.cmdq_seq - 1);
        }
    }

    // Step 10: Reset GSP falcon into RISC-V mode
    log::info("nvidia: gsp: step 10: resetting GSP falcon to RISC-V mode");
    rc = gsp_flcn.reset();
    if (rc != OK) {
        log::error("nvidia: gsp: GSP reset failed: %d", rc);
        return rc;
    }
    // Set RISC-V mode: core_select=RISC-V, valid=1, br_fetch=1
    gsp_flcn.wr32(reg::FALCON_RISCV_BCR_CTRL, 0x00000111);
    delay::us(100);
    log::info("nvidia: gsp: GSP RISCV_BCR_CTRL set to 0x111");

    // Step 10: Write LibOS address to GSP mailboxes
    gsp_flcn.set_mailbox0(static_cast<uint32_t>(state.libos_dma.phys & 0xFFFFFFFF));
    gsp_flcn.set_mailbox1(static_cast<uint32_t>(state.libos_dma.phys >> 32));
    log::info("nvidia: gsp: libos addr written to GSP mailboxes: 0x%lx", state.libos_dma.phys);

    // Step 11: Run SEC2 booter → boots GSP-RM
    rc = gsp_run_booter(gpu, fw, state);
    if (rc != OK) {
        log::error("nvidia: gsp: booter execution failed: %d", rc);
        return rc;
    }

    // Step 12: Wait for INIT_DONE from GSP
    rc = gsp_wait_init_done(gpu, state);
    if (rc != OK) {
        log::error("nvidia: gsp: INIT_DONE wait failed: %d", rc);
        return rc;
    }

    state.initialized = true;

    log::info("nvidia: ========================================");
    log::info("nvidia: GSP-RM BOOT COMPLETE!");
    log::info("nvidia: GSP firmware is running on RISC-V core");
    log::info("nvidia: Message queues operational");
    log::info("nvidia: ========================================");

    return OK;
}

// ============================================================================
// Cleanup
// ============================================================================

void gsp_boot_free(gsp_boot_state& state) {
    // Note: Many DMA buffers must persist while GSP is running.
    // Only free temporary boot allocations here.
    // The radix3, bootloader, sig, libos, log buffers, rmargs, and shm
    // must persist for the lifetime of GSP-RM.

    if (!state.initialized) {
        // If boot failed, free everything
        if (state.wpr_meta_dma.virt) { RUN_ELEVATED(dma::free_pages(state.wpr_meta_dma)); }
        if (state.bootloader_dma.virt) { RUN_ELEVATED(dma::free_pages(state.bootloader_dma)); }
        if (state.sig_dma.virt) { RUN_ELEVATED(dma::free_pages(state.sig_dma)); }
        if (state.libos_dma.virt) { RUN_ELEVATED(dma::free_pages(state.libos_dma)); }
        if (state.loginit_dma.virt) { RUN_ELEVATED(dma::free_pages(state.loginit_dma)); }
        if (state.logintr_dma.virt) { RUN_ELEVATED(dma::free_pages(state.logintr_dma)); }
        if (state.logrm_dma.virt) { RUN_ELEVATED(dma::free_pages(state.logrm_dma)); }
        if (state.rmargs_dma.virt) { RUN_ELEVATED(dma::free_pages(state.rmargs_dma)); }
        if (state.shm_dma.virt) { RUN_ELEVATED(dma::free_pages(state.shm_dma)); }
        if (state.r3.valid) {
            for (int i = 0; i < 3; i++) {
                if (state.r3.lvl[i].virt) { RUN_ELEVATED(dma::free_pages(state.r3.lvl[i])); }
            }
            if (state.r3.fw_data_dma.virt) { RUN_ELEVATED(dma::free_pages(state.r3.fw_data_dma)); }
        }
    }

    string::memset(&state, 0, sizeof(state));
}

} // namespace nv
