#include "drivers/gpu/nvidia/nv_frts.h"
#include "drivers/gpu/nvidia/nv_falcon.h"
#include "drivers/gpu/nvidia/nv_regs.h"
#include "hw/mmio.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace nvidia {

constexpr uint32_t RSA3K_SIG_SIZE = 384; // BCRT30_RSA3K_SIG_SIZE
constexpr uint32_t FWSEC_CMD_FRTS = 0x15; // DMEM_MAPPER_V3_CMD_FRTS
constexpr uint32_t DMEMMAPPER_ENTRY_ID = 0x4;

// little-endian byte helpers over a CPU buffer
static uint32_t buf_rd32(const uint8_t* b, uint32_t o) {
    return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
           (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}
static void buf_wr32(uint8_t* b, uint32_t o, uint32_t v) {
    b[o] = static_cast<uint8_t>(v); b[o + 1] = static_cast<uint8_t>(v >> 8);
    b[o + 2] = static_cast<uint8_t>(v >> 16); b[o + 3] = static_cast<uint8_t>(v >> 24);
}
static uint32_t highest_bit_idx(uint32_t v) {
    uint32_t b = 0;
    while (v >>= 1) b++;
    return b;
}

// Compute frtsOffset top-down from usable FB size (kgspCalculateFbLayout_TU102,
// FRTS slice). NOTE: MMU-lock and WPR-end-margin are assumed absent on this
// consumer GA102; if WPR2 lands wrong we revisit those.
static uint64_t compute_frts_offset(uintptr_t bar0_va) {
    uint32_t mb = 0;
    RUN_ELEVATED(mb = mmio::read32(bar0_va + NV_USABLE_FB_SIZE_IN_MB));
    const uint64_t fbSize       = static_cast<uint64_t>(mb) << 20;
    const uint64_t vgaWorkspace = fbSize - VBIOS_WORKSPACE_SIZE;
    const uint64_t wprEnd       = vgaWorkspace & ~(static_cast<uint64_t>(FB_WPR_ALIGNMENT) - 1);
    const uint64_t frtsOffset   = wprEnd - FRTS_REGION_SIZE;
    log::info("nvidia: frts: fbSize=%u MiB (0x%lx) vgaWs=0x%lx wprEnd=0x%lx frtsOffset=0x%lx",
              mb, static_cast<unsigned long>(fbSize), static_cast<unsigned long>(vgaWorkspace),
              static_cast<unsigned long>(wprEnd), static_cast<unsigned long>(frtsOffset));
    return frtsOffset;
}

// Patch the FRTS command into the FWSEC DMEM (s_vbiosPatchInterfaceData).
static int32_t patch_frts_cmd(uint8_t* dmem, uint32_t dmem_size,
                              uint32_t interfaceOffset, uint64_t frtsOffset) {
    // Build FWSECLIC_FRTS_CMD (44 bytes): readVbiosDesc(24) + frtsRegionDesc(20).
    uint32_t cmd[11];
    cmd[0] = 1;  cmd[1] = 24; cmd[2] = 0; cmd[3] = 0; cmd[4] = 0; cmd[5] = 2; // readVbiosDesc (flags=2)
    cmd[6] = 1;  cmd[7] = 20;                                                 // frtsRegionDesc ver/size
    cmd[8] = static_cast<uint32_t>(frtsOffset >> 12);                         // frtsRegionOffset4K
    cmd[9] = FRTS_REGION_SIZE_4K;                                             // 0x100
    cmd[10] = 2;                                                              // media = FB

    if (interfaceOffset + 4 > dmem_size) return ERR_FRTS_PATCH;
    const uint8_t* hdr = dmem + interfaceOffset;
    const uint8_t entryCount = hdr[3]; // FALCON_APPLICATION_INTERFACE_HEADER_V1.entryCount
    if (entryCount < 2) return ERR_FRTS_PATCH;

    uint32_t cur = interfaceOffset + 4; // entries follow the 4-byte header
    uint32_t mapperOff = 0;
    bool found = false;
    for (uint32_t i = 0; i < entryCount; i++) {
        if (cur + 8 > dmem_size) return ERR_FRTS_PATCH;
        const uint32_t id  = buf_rd32(dmem, cur);
        const uint32_t off = buf_rd32(dmem, cur + 4);
        if (id == DMEMMAPPER_ENTRY_ID) { mapperOff = off; found = true; }
        cur += 8;
    }
    if (!found || mapperOff + 48 > dmem_size) {
        log::error("nvidia: frts: DMEM mapper entry not found");
        return ERR_FRTS_PATCH;
    }

    const uint32_t cmd_in_off = buf_rd32(dmem, mapperOff + 8); // cmd_in_buffer_offset @8
    buf_wr32(dmem, mapperOff + 44, FWSEC_CMD_FRTS);            // init_cmd @44
    if (cmd_in_off + sizeof(cmd) > dmem_size) return ERR_FRTS_PATCH;
    for (uint32_t k = 0; k < sizeof(cmd); k++) {
        dmem[cmd_in_off + k] = reinterpret_cast<const uint8_t*>(cmd)[k];
    }

    log::info("nvidia: frts: patched FRTS cmd (mapper@0x%x init_cmd=0x15 cmd_in@0x%x off4K=0x%x)",
              mapperOff, cmd_in_off, cmd[8]);
    return FRTS_OK;
}

// Select the RSA-3K signature by fuse version and patch it into the DMEM
// (s_executeFwsec_TU102 bit-walk). Returns FRTS_OK on success.
static int32_t patch_signature(uintptr_t bar0_va, const vbios_image& vb,
                               const fwsec_info& fi, fwsec_ucode& ucode) {
    const uint32_t index = (fi.ucode_id > 0) ? (fi.ucode_id - 1) : 0;
    uint32_t fuseVal = 0;
    RUN_ELEVATED(fuseVal = mmio::read32(bar0_va + NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION + 4 * index));

    uint32_t fuseVer = 0;
    if (fuseVal) fuseVer = highest_bit_idx(fuseVal) + 1;
    uint32_t ucodeVersionVal = 1u << fuseVer;
    uint32_t hsSigVersions   = fi.signature_versions;

    if ((ucodeVersionVal & hsSigVersions) == 0) {
        log::error("nvidia: frts: no signature for fuse ver %u (fuse=0x%x sigVers=0x%x)",
                   fuseVer, fuseVal, hsSigVersions);
        return ERR_FRTS_SIG;
    }

    uint32_t sigOffset = 0;
    uint32_t uvv = ucodeVersionVal, hsv = hsSigVersions;
    while ((uvv & hsv & 1u) == 0) {
        sigOffset += (hsv & 1u) * RSA3K_SIG_SIZE;
        hsv >>= 1;
        uvv >>= 1;
    }

    const uint32_t sigTotal = fi.signature_count * RSA3K_SIG_SIZE;
    if (sigOffset + RSA3K_SIG_SIZE > sigTotal) {
        log::error("nvidia: frts: sig offset 0x%x out of range (total %u)", sigOffset, sigTotal);
        return ERR_FRTS_SIG;
    }

    // Signatures live in the VBIOS right after the 44-byte V3 descriptor header.
    const uint8_t* sigs = vb.image + fi.desc_offset + 44;
    // Patch the selected signature into the DMEM at PKCDataOffset.
    uint8_t* dmem = reinterpret_cast<uint8_t*>(ucode.image.cpu_va) + ucode.imem_size;
    const uint32_t dst = fi.pkc_data_offset;
    if (dst + RSA3K_SIG_SIZE > ucode.dmem_size) {
        log::error("nvidia: frts: sig dst 0x%x out of DMEM (%u)", dst, ucode.dmem_size);
        return ERR_FRTS_SIG;
    }
    for (uint32_t k = 0; k < RSA3K_SIG_SIZE; k++) dmem[dst + k] = sigs[sigOffset + k];

    log::info("nvidia: frts: fuse=0x%x -> fuseVer=%u, selected sig #%u (off 0x%x) -> DMEM 0x%x",
              fuseVal, fuseVer, sigOffset / RSA3K_SIG_SIZE, sigOffset, dst);
    return FRTS_OK;
}

int32_t fwsec_frts_execute(uintptr_t bar0_va, uint32_t chip_id,
                           const vbios_image& vb, const fwsec_info& fi,
                           fwsec_ucode& ucode) {
    // 1) Compute frtsOffset from the FB layout.
    const uint64_t frtsOffset = compute_frts_offset(bar0_va);

    // 2) Patch the FWSEC DMEM (FRTS command + the fuse-selected signature).
    uint8_t* dmem = reinterpret_cast<uint8_t*>(ucode.image.cpu_va) + ucode.imem_size;
    int32_t rc = patch_frts_cmd(dmem, ucode.dmem_size, fi.interface_offset, frtsOffset);
    if (rc != FRTS_OK) return rc;
    rc = patch_signature(bar0_va, vb, fi, ucode);
    if (rc != FRTS_OK) return rc;

    // 3) Reset the GSP Falcon, then run FWSEC. *** FIRST GPU WRITES ***
    const falcon f = { bar0_va, NV_PGSP_FALCON_BASE, NV_FALCON2_GSP_BASE, NV_PGSP_FBIF_BASE };
    log::info("nvidia: frts: resetting GSP Falcon (chipId=0x%08x)...", chip_id);
    falcon_reset(f, chip_id);

    flcn_hs_ucode hu;
    hu.ucode_phys       = ucode.image.phys;
    hu.imem_pa          = fi.imem_phys_base;
    hu.imem_va          = fi.imem_virt_base;
    hu.imem_size        = ucode.imem_size;
    hu.dmem_pa          = fi.dmem_phys_base;
    hu.data_offset      = ucode.imem_size;
    hu.dmem_size        = ucode.dmem_size;
    hu.hs_sig_dmem_addr = fi.pkc_data_offset;
    hu.engine_id_mask   = fi.engine_id_mask;
    hu.ucode_id         = fi.ucode_id;

    log::info("nvidia: frts: running FWSEC (ucode phys=0x%lx imem=%u dmem=%u)...",
              static_cast<unsigned long>(hu.ucode_phys), hu.imem_size, hu.dmem_size);
    const int32_t ex = falcon_hs_execute(f, hu);
    if (ex != FLCN_OK) {
        log::error("nvidia: frts: FWSEC Falcon execute failed rc=%d (no halt?)", ex);
        return ERR_FRTS_EXEC;
    }

    // 4) Verify FRTS error code + that WPR2 was carved.
    uint32_t scratch = 0, wpr2hi = 0, wpr2lo = 0;
    RUN_ELEVATED({
        scratch = mmio::read32(bar0_va + NV_PBUS_VBIOS_SCRATCH_0E);
        wpr2hi  = mmio::read32(bar0_va + NV_PFB_PRI_MMU_WPR2_ADDR_HI);
        wpr2lo  = mmio::read32(bar0_va + NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    });
    const uint32_t frtsErr   = (scratch >> 16) & 0xFFFF;
    const uint32_t hiVal     = wpr2hi >> NV_PFB_WPR2_ADDR_SHIFT;
    const uint32_t loVal     = wpr2lo >> NV_PFB_WPR2_ADDR_SHIFT;
    const uint32_t expectLo  = static_cast<uint32_t>(frtsOffset >> NV_PFB_WPR2_ADDR_ALIGNMENT);

    log::info("nvidia: frts: scratch0E=0x%08x frtsErr=0x%x | WPR2_HI=0x%08x WPR2_LO=0x%08x (loVal=0x%x expect=0x%x)",
              scratch, frtsErr, wpr2hi, wpr2lo, loVal, expectLo);

    if (frtsErr != 0) {
        log::error("nvidia: frts: FWSEC FRTS error code 0x%x", frtsErr);
        return ERR_FRTS_WPR2;
    }
    if (hiVal == 0) {
        log::error("nvidia: frts: WPR2 not carved (WPR2_ADDR_HI val == 0)");
        return ERR_FRTS_WPR2;
    }

    log::info("nvidia: frts: *** WPR2 CARVED - FWSEC-FRTS SUCCESS (boot-stage-05) ***");
    return FRTS_OK;
}

} // namespace nvidia
