#include "drivers/graphics/nvidia/nv_fwsec.h"
#include "drivers/graphics/nvidia/nv_gpu.h"
#include "hw/delay.h"
#include "clock/clock.h"
#include "dma/dma.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

namespace nv {

// ============================================================================
// Helper: find-last-set bit (equivalent to Linux fls())
// Returns 1-based bit position of highest set bit, 0 if no bits set.
// ============================================================================

static uint32_t fls32(uint32_t val) {
    if (val == 0) return 0;
    uint32_t pos = 32;
    if (!(val & 0xFFFF0000u)) { pos -= 16; val <<= 16; }
    if (!(val & 0xFF000000u)) { pos -= 8;  val <<= 8; }
    if (!(val & 0xF0000000u)) { pos -= 4;  val <<= 4; }
    if (!(val & 0xC0000000u)) { pos -= 2;  val <<= 2; }
    if (!(val & 0x80000000u)) { pos -= 1; }
    return pos;
}

// Popcount (count set bits)
static uint32_t popcount32(uint32_t val) {
    val = val - ((val >> 1) & 0x55555555u);
    val = (val & 0x33333333u) + ((val >> 2) & 0x33333333u);
    return ((val + (val >> 4)) & 0x0F0F0F0Fu) * 0x01010101u >> 24;
}

// ============================================================================
// FRTS Region Computation
// ============================================================================

int32_t fwsec_compute_frts_region(nv_gpu* gpu, uint64_t& out_addr, uint64_t& out_size) {
    // Read VRAM size (GA102+: register 0x1183a4, value << 20)
    uint32_t vidmem_reg = gpu->reg_rd32(reg::VIDMEM_SIZE_GA102);
    uint64_t fb_size = static_cast<uint64_t>(vidmem_reg) << 20;

    if (fb_size == 0) {
        log::error("nvidia: fwsec: VRAM size register reads 0");
        return ERR_INVALID;
    }

    log::info("nvidia: fwsec: VRAM size: %lu MB (%lu bytes, reg=0x%08x)",
              fb_size / (1024 * 1024), fb_size, vidmem_reg);

    // VGA workspace is at the top of VRAM
    // Check NV_PDISP_VGA_WORKSPACE_BASE register, or fall back to fb_size - 0x100000
    uint32_t vga_reg = gpu->reg_rd32(reg::VGA_WORKSPACE_BASE);
    uint64_t vga_base;

    // VGA workspace register uses the same encoding as VBIOS instance ptr
    // bits[31:8] << 8 gives the address, but it's actually a FB-relative offset
    // If the register reads 0 or has VBIOS instance encoding, use default
    if (vga_reg != 0 && (vga_reg & 0x08)) {
        // This is actually the VBIOS instance pointer register, not VGA workspace
        // For GA102, VGA workspace is typically at fb_size - 0x100000
        vga_base = fb_size - 0x100000;
    } else {
        vga_base = fb_size - 0x100000;
    }

    log::info("nvidia: fwsec: VGA workspace base: 0x%lx", vga_base);

    // FRTS region: 128KB-aligned, below VGA workspace, 1MB size
    // frts_base = ALIGN_DOWN(vga_base, 0x20000) - FRTS_REGION_SIZE
    uint64_t frts_end = vga_base & ~0x1FFFFull; // Align down to 128KB
    uint64_t frts_base = frts_end - FRTS_REGION_SIZE;

    out_addr = frts_base;
    out_size = FRTS_REGION_SIZE;

    log::info("nvidia: fwsec: FRTS region: 0x%lx - 0x%lx (%lu bytes)",
              frts_base, frts_end, out_size);

    return OK;
}

// ============================================================================
// Signature Selection
// ============================================================================

int32_t fwsec_select_signature(nv_gpu* gpu, const fwsec_fw& fwsec, uint32_t& out_sig_idx) {
    // Determine fuse register address based on engine_id_mask
    uint32_t fuse_base;
    if (fwsec.desc.engine_id_mask & 0x0400) {
        fuse_base = reg::FUSE_GSP_BASE; // GSP: 0x8241c0
    } else if (fwsec.desc.engine_id_mask & 0x0001) {
        fuse_base = reg::FUSE_SEC2_BASE; // SEC2: 0x824140
    } else {
        log::error("nvidia: fwsec: unknown engine_id_mask 0x%04x", fwsec.desc.engine_id_mask);
        return ERR_UNSUPPORTED;
    }

    // ucode_id is 1-indexed
    uint32_t fuse_addr = fuse_base + (fwsec.desc.ucode_id - 1) * 4;
    uint32_t fuse_val = gpu->reg_rd32(fuse_addr);

    log::info("nvidia: fwsec: fuse register 0x%06x = 0x%08x (engine_mask=0x%04x ucode_id=%u)",
              fuse_addr, fuse_val, fwsec.desc.engine_id_mask, fwsec.desc.ucode_id);

    // Compute effective fuse version: position of highest set bit (1-based)
    uint32_t reg_fuse_version = fls32(fuse_val);

    log::info("nvidia: fwsec: reg_fuse_version = %u (fls of 0x%08x)", reg_fuse_version, fuse_val);

    // Check the firmware's signature_versions bitmask
    // The bit to check is 1 << fls(fuse_val), matching nouveau's BIT(fls(reg_fuse_version))
    // and nova-core's (1 << reg_fuse_version)
    uint16_t sig_versions = fwsec.desc.signature_versions;
    uint32_t reg_fuse_bit = 1u << reg_fuse_version;

    log::info("nvidia: fwsec: signature_versions bitmask = 0x%04x, target bit = 0x%08x",
              sig_versions, reg_fuse_bit);

    if (!(sig_versions & reg_fuse_bit)) {
        log::error("nvidia: fwsec: signature version %u not available in firmware (versions=0x%04x)",
                   reg_fuse_version, sig_versions);
        return ERR_NOT_FOUND;
    }

    // Count set bits below the target bit to get the index into the signatures array
    uint32_t mask_below = reg_fuse_bit - 1;
    out_sig_idx = popcount32(sig_versions & mask_below);

    log::info("nvidia: fwsec: selected signature index %u (of %u total)", out_sig_idx, fwsec.sig_count);

    if (out_sig_idx >= fwsec.sig_count) {
        log::error("nvidia: fwsec: signature index %u out of range (%u available)",
                   out_sig_idx, fwsec.sig_count);
        return ERR_INVALID;
    }

    return OK;
}

// ============================================================================
// DMEM Patching
// ============================================================================

int32_t fwsec_patch_dmem(fwsec_fw& fwsec, uint64_t frts_addr, uint64_t frts_size,
                          uint32_t sig_idx) {
    uint8_t* ucode = fwsec.ucode_data;
    uint32_t imem_size = fwsec.desc.imem_load_size;
    uint32_t ucode_size = fwsec.ucode_size;

    log::info("nvidia: fwsec: patching DMEM (imem_size=%u, interface_off=0x%x)",
              imem_size, fwsec.desc.interface_offset);

    // Step 1: Find FalconAppifHdrV1 at imem_load_size + interface_offset
    uint32_t appif_off = imem_size + fwsec.desc.interface_offset;
    if (appif_off + sizeof(falcon_appif_hdr_v1) > ucode_size) {
        log::error("nvidia: fwsec: app interface header out of bounds (off=0x%x, size=%u)",
                   appif_off, ucode_size);
        return ERR_INVALID;
    }

    falcon_appif_hdr_v1 appif_hdr;
    string::memcpy(&appif_hdr, ucode + appif_off, sizeof(appif_hdr));

    log::info("nvidia: fwsec: AppifHdr: version=%u entries=%u entry_size=%u",
              appif_hdr.version, appif_hdr.entry_count, appif_hdr.entry_size);

    if (appif_hdr.version != 1) {
        log::error("nvidia: fwsec: unexpected appif version %u (expected 1)", appif_hdr.version);
        return ERR_INVALID;
    }

    // Step 2: Find DMEMMAPPER entry (id = 0x04)
    uint32_t dmem_mapper_off = 0;
    bool found = false;

    for (uint8_t i = 0; i < appif_hdr.entry_count; i++) {
        uint32_t entry_off = appif_off + appif_hdr.header_size + i * appif_hdr.entry_size;
        if (entry_off + sizeof(falcon_appif_v1) > ucode_size) break;

        falcon_appif_v1 entry;
        string::memcpy(&entry, ucode + entry_off, sizeof(entry));

        log::info("nvidia: fwsec: AppifEntry[%u]: id=0x%x dmem_base=0x%x", i, entry.id, entry.dmem_base);

        if (entry.id == APPIF_ID_DMEMMAPPER) {
            dmem_mapper_off = imem_size + entry.dmem_base;
            found = true;
            break;
        }
    }

    if (!found) {
        log::error("nvidia: fwsec: DMEMMAPPER entry (id=0x04) not found");
        return ERR_NOT_FOUND;
    }

    // Step 3: Read and validate DMEMMAPPER
    if (dmem_mapper_off + sizeof(falcon_dmemmapper_v3) > ucode_size) {
        log::error("nvidia: fwsec: DMEMMAPPER out of bounds (off=0x%x)", dmem_mapper_off);
        return ERR_INVALID;
    }

    falcon_dmemmapper_v3 dmapper;
    string::memcpy(&dmapper, ucode + dmem_mapper_off, sizeof(dmapper));

    log::info("nvidia: fwsec: DMEMMAPPER: sig=0x%08x ver=%u size=%u cmd_in_off=0x%x cmd_in_size=%u",
              dmapper.signature, dmapper.version, dmapper.size,
              dmapper.cmd_in_buffer_offset, dmapper.cmd_in_buffer_size);

    if (dmapper.signature != DMEMMAPPER_SIG) {
        log::error("nvidia: fwsec: DMEMMAPPER signature mismatch: 0x%08x (expected 0x%08x)",
                   dmapper.signature, DMEMMAPPER_SIG);
        return ERR_INVALID;
    }

    // Step 4: Patch init_cmd to FRTS command
    dmapper.init_cmd = FWSEC_CMD_FRTS;
    string::memcpy(ucode + dmem_mapper_off, &dmapper, sizeof(dmapper));
    log::info("nvidia: fwsec: patched init_cmd = 0x%x (FRTS)", FWSEC_CMD_FRTS);

    // Step 5: Patch FRTS command input buffer
    uint32_t cmd_in_off = imem_size + dmapper.cmd_in_buffer_offset;
    if (cmd_in_off + sizeof(frts_cmd) > ucode_size) {
        log::error("nvidia: fwsec: FRTS command buffer out of bounds (off=0x%x)", cmd_in_off);
        return ERR_INVALID;
    }

    frts_cmd cmd;
    string::memset(&cmd, 0, sizeof(cmd));

    // ReadVbios: tell FWSEC to read VBIOS from ROM itself
    cmd.read_vbios.ver   = 1;
    cmd.read_vbios.hdr   = sizeof(frts_read_vbios); // 24
    cmd.read_vbios.addr  = 0; // GPU reads from ROM
    cmd.read_vbios.size  = 0;
    cmd.read_vbios.flags = 2;

    // FrtsRegion: where to create the FRTS/WPR2 region in VRAM
    cmd.region.ver   = 1;
    cmd.region.hdr   = sizeof(frts_region); // 20
    cmd.region.addr  = static_cast<uint32_t>(frts_addr >> 12); // Page-aligned
    cmd.region.size  = static_cast<uint32_t>(frts_size >> 12); // In pages
    cmd.region.ftype = FRTS_REGION_TYPE_FB;

    string::memcpy(ucode + cmd_in_off, &cmd, sizeof(cmd));

    log::info("nvidia: fwsec: FRTS cmd: vbios={ver=%u,hdr=%u,addr=0x%lx,size=0x%x,flags=%u}",
              cmd.read_vbios.ver, cmd.read_vbios.hdr,
              cmd.read_vbios.addr, cmd.read_vbios.size, cmd.read_vbios.flags);
    log::info("nvidia: fwsec: FRTS cmd: region={ver=%u,hdr=%u,addr=0x%x(>>12=0x%lx),size=0x%x,type=%u}",
              cmd.region.ver, cmd.region.hdr,
              cmd.region.addr, frts_addr, cmd.region.size, cmd.region.ftype);

    // Step 6: Copy selected signature into ucode at pkc_data_offset
    uint32_t pkc_off = imem_size + fwsec.desc.pkc_data_offset;
    uint32_t sig_src = sig_idx * RSA3K_SIG_SIZE;

    if (pkc_off + RSA3K_SIG_SIZE > ucode_size) {
        log::error("nvidia: fwsec: PKC data offset out of bounds (off=0x%x)", pkc_off);
        return ERR_INVALID;
    }
    if (sig_src + RSA3K_SIG_SIZE > fwsec.sig_count * RSA3K_SIG_SIZE) {
        log::error("nvidia: fwsec: signature index %u out of range", sig_idx);
        return ERR_INVALID;
    }

    string::memcpy(ucode + pkc_off, fwsec.signatures + sig_src, RSA3K_SIG_SIZE);
    log::info("nvidia: fwsec: patched RSA-3K signature %u (%u bytes) at DMEM offset 0x%x",
              sig_idx, RSA3K_SIG_SIZE, fwsec.desc.pkc_data_offset);

    return OK;
}

// ============================================================================
// FWSEC-FRTS Execution
// ============================================================================

int32_t fwsec_run_frts(nv_gpu* gpu, fwsec_fw& fwsec, falcon& gsp) {
    log::info("nvidia: ========================================");
    log::info("nvidia: Phase B: FWSEC-FRTS execution");
    log::info("nvidia: ========================================");

    int32_t rc;

    // Step 0: Check if WPR2 already exists (would indicate stale GPU state)
    uint32_t wpr2_hi_check = gpu->reg_rd32(reg::WPR2_ADDR_HI);
    if ((wpr2_hi_check & 0xFFFFFFF0u) != 0) {
        log::warn("nvidia: fwsec: WPR2 already exists (HI=0x%08x) — GPU may need full reset",
                  wpr2_hi_check);
        // Don't error out — the WPR2 may be from a previous boot and FWSEC will re-establish it
    }

    // Step 1: Compute FRTS region in VRAM
    uint64_t frts_addr = 0, frts_size = 0;
    rc = fwsec_compute_frts_region(gpu, frts_addr, frts_size);
    if (rc != OK) {
        log::error("nvidia: fwsec: FRTS region computation failed: %d", rc);
        return rc;
    }

    // Step 2: Select the correct signature
    uint32_t sig_idx = 0;
    rc = fwsec_select_signature(gpu, fwsec, sig_idx);
    if (rc != OK) {
        log::error("nvidia: fwsec: signature selection failed: %d", rc);
        return rc;
    }

    // Step 3: Patch DMEM with FRTS command and signature
    rc = fwsec_patch_dmem(fwsec, frts_addr, frts_size, sig_idx);
    if (rc != OK) {
        log::error("nvidia: fwsec: DMEM patching failed: %d", rc);
        return rc;
    }

    // Step 4: Allocate DMA buffer for the patched ucode
    uint32_t dma_pages = (fwsec.ucode_size + 4095) / 4096;
    rc = 0;
    RUN_ELEVATED(rc = dma::alloc_pages(dma_pages, fwsec.dma, pmm::ZONE_DMA32, paging::PAGE_USER));
    if (rc != dma::OK) {
        log::error("nvidia: fwsec: DMA allocation failed (%u pages): %d", dma_pages, rc);
        return ERR_NOT_FOUND;
    }
    fwsec.dma_valid = true;

    // Copy patched ucode to DMA buffer
    string::memcpy(reinterpret_cast<void*>(fwsec.dma.virt),
                   fwsec.ucode_data, fwsec.ucode_size);

    log::info("nvidia: fwsec: DMA buffer: virt=0x%lx phys=0x%lx size=%u",
              fwsec.dma.virt, fwsec.dma.phys, fwsec.ucode_size);

    // Step 5: Reset GSP falcon
    rc = gsp.reset();
    if (rc != OK) {
        log::error("nvidia: fwsec: GSP falcon reset failed: %d", rc);
        return rc;
    }

    // Step 6: DMA load IMEM + DMEM onto GSP falcon
    uint32_t imem_size = fwsec.desc.imem_load_size;
    uint32_t dmem_size = fwsec.desc.dmem_load_size;
    uint32_t dmem_aligned = (dmem_size + (DMEM_ALIGN - 1)) & ~(DMEM_ALIGN - 1);

    rc = gsp.dma_load(fwsec.dma,
                      fwsec.desc.imem_phys_base, // IMEM destination
                      0,                          // IMEM source offset in DMA buf
                      imem_size,
                      fwsec.desc.dmem_phys_base, // DMEM destination
                      imem_size,                  // DMEM source offset (after IMEM)
                      dmem_aligned,
                      true);                      // Secure IMEM
    if (rc != OK) {
        log::error("nvidia: fwsec: DMA load failed: %d", rc);
        return rc;
    }

    // Step 7: Program BROM registers
    rc = gsp.program_brom(fwsec.desc.pkc_data_offset,
                          fwsec.desc.engine_id_mask,
                          fwsec.desc.ucode_id);
    if (rc != OK) return rc;

    // Step 8: Boot and wait
    uint32_t mbox0_out = 0, mbox1_out = 0;
    rc = gsp.boot_and_wait(0, 0, 0, mbox0_out, mbox1_out);
    if (rc != OK) {
        log::error("nvidia: fwsec: GSP boot failed: %d", rc);
        return rc;
    }

    // Step 9: Verify FWSEC-FRTS success
    // Check mailbox0 return value (should be 0 on success)
    if (mbox0_out != 0) {
        log::error("nvidia: fwsec: FWSEC-FRTS mailbox0 error: 0x%08x", mbox0_out);
        return ERR_IO;
    }

    uint32_t scratch_e = gpu->reg_rd32(reg::FWSEC_SCRATCH_E);
    uint32_t error_code = scratch_e >> 16;

    log::info("nvidia: fwsec: SCRATCH_E = 0x%08x (error code = 0x%04x)", scratch_e, error_code);

    if (error_code != 0) {
        log::error("nvidia: fwsec: FWSEC-FRTS FAILED with error code 0x%04x", error_code);
        return ERR_IO;
    }

    // Step 10: Read WPR2 bounds
    uint32_t wpr2_lo = gpu->reg_rd32(reg::WPR2_ADDR_LO);
    uint32_t wpr2_hi = gpu->reg_rd32(reg::WPR2_ADDR_HI);
    uint64_t wpr2_start = (static_cast<uint64_t>(wpr2_lo) & 0xFFFFFFF0u) << 8;
    uint64_t wpr2_end   = (static_cast<uint64_t>(wpr2_hi) & 0xFFFFFFF0u) << 8;

    log::info("nvidia: ========================================");
    log::info("nvidia: FWSEC-FRTS SUCCESS!");
    log::info("nvidia: WPR2 region: 0x%lx - 0x%lx (%lu KB)",
              wpr2_start, wpr2_end, (wpr2_end - wpr2_start) / 1024);
    log::info("nvidia: FRTS region: 0x%lx (%lu KB)",
              frts_addr, frts_size / 1024);
    log::info("nvidia: ========================================");

    return OK;
}

} // namespace nv
