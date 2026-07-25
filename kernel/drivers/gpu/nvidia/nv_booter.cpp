#include "drivers/gpu/nvidia/nv_booter.h"
#include "drivers/gpu/nvidia/nv_falcon.h"
#include "drivers/gpu/nvidia/nv_regs.h"
#include "fs/fs.h"
#include "hw/mmio.h"
#include "hw/barrier.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"
#include "common/string.h"

namespace nvidia {

constexpr const char* BL_IMAGE      = "/firmware/nvidia/535.183.01/booter_load_image.bin";
constexpr const char* BL_HEADER     = "/firmware/nvidia/535.183.01/booter_load_header.bin";
constexpr const char* BL_SIG        = "/firmware/nvidia/535.183.01/booter_load_sig.bin";
constexpr const char* BL_PATCH_LOC  = "/firmware/nvidia/535.183.01/booter_load_patch_loc.bin";
constexpr const char* BL_PATCH_META = "/firmware/nvidia/535.183.01/booter_load_patch_meta.bin";
constexpr const char* BL_NUM_SIGS   = "/firmware/nvidia/535.183.01/booter_load_num_sigs.bin";

static int32_t read_file(const char* path, void* dst, size_t expect) {
    int32_t oerr = 0;
    fs::file* f = fs::open(path, fs::O_RDONLY, &oerr);
    if (!f) {
        log::error("nvidia: booter: open(%s) failed err=%d", path, oerr);
        return ERR_BOOTER_IO;
    }
    const ssize_t got = fs::read(f, dst, expect);
    fs::close(f);
    if (got != static_cast<ssize_t>(expect)) {
        log::error("nvidia: booter: read(%s) got=%ld want=%lu", path,
                   static_cast<long>(got), static_cast<unsigned long>(expect));
        return ERR_BOOTER_SIZE;
    }
    return BOOTER_OK;
}

int32_t gsp_load_signature(const gsp_fw_image& fw, dma_buffer& out) {
    out = dma_buffer{};
    if (fw.sig_ga10x_size == 0) {
        log::error("nvidia: booter: .fwsignature_ga10x not located");
        return ERR_BOOTER_SIG;
    }
    const size_t aligned = (static_cast<size_t>(fw.sig_ga10x_size) + 255) & ~static_cast<size_t>(255);
    if (dma_alloc(aligned, /*uncached=*/false, out) != MEM_OK) {
        return ERR_BOOTER_MEM;
    }
    int32_t oerr = 0;
    fs::file* f = fs::open(GSP_FW_PATH, fs::O_RDONLY, &oerr);
    if (!f) { dma_free(out); return ERR_BOOTER_IO; }
    bool ok = fs::seek(f, static_cast<int64_t>(fw.sig_ga10x_off), fs::SEEK_SET) >= 0;
    if (ok) {
        const ssize_t got = fs::read(f, reinterpret_cast<void*>(out.cpu_va), fw.sig_ga10x_size);
        ok = (got == static_cast<ssize_t>(fw.sig_ga10x_size));
    }
    fs::close(f);
    if (!ok) { dma_free(out); return ERR_BOOTER_IO; }
    log::info("nvidia: booter: GSP-RM signature %lu B @ phys=0x%lx",
              static_cast<unsigned long>(fw.sig_ga10x_size), static_cast<unsigned long>(out.phys));
    return BOOTER_OK;
}

int32_t booter_load_execute(uintptr_t bar0_va, uint32_t chip_id, uint64_t wprmeta_phys) {
    int32_t rc;

    // ---- metadata (header 9xu32; patch_loc, num_sigs u32; patch_meta 3xu32) ----
    uint32_t header[9];
    if ((rc = read_file(BL_HEADER, header, sizeof(header))) != BOOTER_OK) return rc;
    uint32_t patchLoc = 0, numSigs = 0, patchMeta[3];
    if ((rc = read_file(BL_PATCH_LOC, &patchLoc, sizeof(patchLoc))) != BOOTER_OK) return rc;
    if ((rc = read_file(BL_NUM_SIGS, &numSigs, sizeof(numSigs))) != BOOTER_OK) return rc;
    if ((rc = read_file(BL_PATCH_META, patchMeta, sizeof(patchMeta))) != BOOTER_OK) return rc;

    const uint32_t osDataOffset  = header[2];
    const uint32_t osDataSize    = header[3];
    const uint32_t appCodeOffset = header[5];
    const uint32_t appCodeSize   = header[6];
    const uint32_t engineId      = patchMeta[1];
    const uint32_t ucodeId       = patchMeta[2];

    if (numSigs == 0) { log::error("nvidia: booter: numSigs=0"); return ERR_BOOTER_SIG; }

    // ---- signatures (fixed 2KB scratch; Booter Load sig blob is 768 B) ----
    fs::vattr sattr;
    if (fs::stat(BL_SIG, &sattr) != fs::OK) { log::error("nvidia: booter: sig stat failed"); return ERR_BOOTER_IO; }
    const uint32_t sigTotal = static_cast<uint32_t>(sattr.size);
    uint8_t sigs[2048];
    if (sigTotal == 0 || sigTotal > sizeof(sigs) || (sigTotal % numSigs) != 0) {
        log::error("nvidia: booter: bad sig blob size=%u numSigs=%u", sigTotal, numSigs);
        return ERR_BOOTER_SIG;
    }
    if ((rc = read_file(BL_SIG, sigs, sigTotal)) != BOOTER_OK) return rc;
    const uint32_t sigSize = sigTotal / numSigs;

    // ---- SEC2 fuse version -> signature index (numSigs-1-fuseVer) ----
    const uint32_t index = ucodeId - 1;
    uint32_t fuse = 0;
    RUN_ELEVATED(fuse = mmio::read32(bar0_va + NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION + 4 * index));
    uint32_t fuseVer = 0;
    if (fuse != 0) {
        uint32_t hbi = 0;
        for (int b = 31; b >= 0; b--) { if (fuse & (1u << b)) { hbi = static_cast<uint32_t>(b); break; } }
        fuseVer = hbi + 1;
    }
    uint32_t sigIndex = 0;
    if (numSigs > 1) {
        if (fuseVer > numSigs - 1) {
            log::error("nvidia: booter: no signature for fuseVer=%u (numSigs=%u)", fuseVer, numSigs);
            return ERR_BOOTER_SIG;
        }
        sigIndex = numSigs - 1 - fuseVer;
    }

    // ---- image into an UNCACHED contiguous DMA buffer (matches BOOT_FROM_HS) ----
    fs::vattr iattr;
    if (fs::stat(BL_IMAGE, &iattr) != fs::OK) { log::error("nvidia: booter: image stat failed"); return ERR_BOOTER_IO; }
    const uint32_t imageSize = static_cast<uint32_t>(iattr.size);
    dma_buffer image;
    if (dma_alloc(imageSize, /*uncached=*/true, image) != MEM_OK) return ERR_BOOTER_MEM;
    if ((rc = read_file(BL_IMAGE, reinterpret_cast<void*>(image.cpu_va), imageSize)) != BOOTER_OK) {
        dma_free(image);
        return rc;
    }
    if (patchLoc + sigSize > imageSize) {
        log::error("nvidia: booter: patchLoc 0x%x + sigSize %u > image %u", patchLoc, sigSize, imageSize);
        dma_free(image);
        return ERR_BOOTER_SIG;
    }
    string::memcpy(reinterpret_cast<void*>(image.cpu_va + patchLoc), sigs + sigIndex * sigSize, sigSize);
    barrier::dma_full();

    log::info("nvidia: booter: image=%u B @ phys=0x%lx ucodeId=%u engId=0x%x numSigs=%u fuse=0x%x fuseVer=%u sigIdx=%u",
              imageSize, static_cast<unsigned long>(image.phys), ucodeId, engineId,
              numSigs, fuse, fuseVer, sigIndex);
    log::info("nvidia: booter: appCode @0x%x/0x%x osData @0x%x/0x%x patchLoc=0x%x hsSigDmem=0x%x",
              appCodeOffset, appCodeSize, osDataOffset, osDataSize, patchLoc, patchLoc - osDataOffset);

    // ---- build SEC2 falcon + BOOT_FROM_HS ucode params ----
    falcon sec2;
    sec2.bar0_va    = bar0_va;
    sec2.reg_base   = NV_PSEC_FALCON_BASE;
    sec2.riscv_base = NV_FALCON2_SEC_BASE;
    sec2.fbif_base  = NV_PSEC_FBIF_BASE;

    flcn_hs_ucode u;
    u.ucode_phys       = image.phys;
    u.imem_pa          = 0;
    u.imem_va          = appCodeOffset;
    u.imem_size        = appCodeSize;
    u.dmem_pa          = 0;
    u.data_offset      = osDataOffset;
    u.dmem_size        = osDataSize;
    u.hs_sig_dmem_addr = patchLoc - osDataOffset;
    u.engine_id_mask   = engineId;
    u.ucode_id         = ucodeId;

    // ---- reset SEC2 + execute with MAILBOX = WprMeta phys ----
    log::info("nvidia: booter: resetting SEC2 + executing Booter Load (wprMeta=0x%lx)...",
              static_cast<unsigned long>(wprmeta_phys));
    falcon_reset(sec2, chip_id);
    uint32_t mb0 = static_cast<uint32_t>(wprmeta_phys & 0xFFFFFFFFu);
    uint32_t mb1 = static_cast<uint32_t>(wprmeta_phys >> 32);
    const int32_t erc = falcon_hs_execute(sec2, u, &mb0, &mb1);
    dma_free(image);

    if (erc != FLCN_OK) {
        log::error("nvidia: booter: SEC2 Booter Load did not halt (rc=%d)", erc);
        return ERR_BOOTER_EXEC;
    }
    if (mb0 != 0) {
        log::error("nvidia: booter: Booter Load FAILED - mailbox0=0x%x (expected 0)", mb0);
        return ERR_BOOTER_MAILBOX;
    }
    log::info("nvidia: booter: *** Booter Load SUCCESS (mailbox0=0) - GSP-RM authenticated into WPR2 ***");
    return BOOTER_OK;
}

} // namespace nvidia
