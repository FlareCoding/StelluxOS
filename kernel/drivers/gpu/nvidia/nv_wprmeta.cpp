#include "drivers/gpu/nvidia/nv_wprmeta.h"
#include "drivers/gpu/nvidia/nv_regs.h"
#include "hw/mmio.h"
#include "hw/barrier.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"

namespace nvidia {

static inline uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static inline uint64_t align_up(uint64_t v, uint64_t a)   { return (v + a - 1) & ~(a - 1); }

// GA102 GSP-RM WPR heap size: kgspGetFwHeapSize -> _kgspCalculateFwHeapSize for
// the baremetal (PF_KERNEL_ONLY, LIBOS3) path with no scrubber ucode and no
// registry override. Constants from gsp_fw_heap.h + the GA102 HAL dispatch:
//   OsCarveout(LIBOS3)=20MiB, Base(GA10x)=8MiB, 96KiB/GB, client=96MiB,
//   min=84MiB, prescrubbed top-of-FB=256MiB, no scrubber -> heap capped to it.
static uint64_t calc_fw_heap_size(uint64_t fbSize, uint64_t gspFwOffset) {
    constexpr uint64_t MiB = 1ull << 20;
    const uint64_t fbSizeGB = align_up(fbSize, 1ull << 30) >> 30;

    uint64_t heap = (20ull * MiB) + (8ull * MiB)
                  + align_up((96ull << 10) * fbSizeGB, MiB)
                  + align_up((48ull << 10) * 2048ull, MiB);

    constexpr uint64_t minMB = 84;                  // kgspGetMinWprHeapSizeMB (baremetal)
    constexpr uint64_t prescrubbed = 256ull * MiB;  // kgspGetPrescrubbedTopFbSize (GA102)
    const uint64_t posteriorFbSize = fbSize - gspFwOffset;
    uint64_t maxMB = 0xFFFFFFFFull;                 // NV_U32_MAX when unbounded
    if (posteriorFbSize != 0 && prescrubbed > posteriorFbSize) {
        maxMB = (prescrubbed - posteriorFbSize) >> 20;
    }
    if (heap < minMB * MiB) heap = minMB * MiB;
    if (heap > maxMB * MiB) heap = maxMB * MiB;
    return heap;
}

int32_t gsp_build_wprmeta(uintptr_t bar0_va, const gsp_radix3& radix3,
                          const gsp_boot_ucode& boot, gsp_wprmeta& out) {
    out.buf = dma_buffer{};
    out.meta = nullptr;

    // Usable FB size + the WPR2 the FWSEC-FRTS stage already carved (cross-check).
    uint32_t mb = 0, wpr2_lo = 0, wpr2_hi = 0;
    RUN_ELEVATED({
        mb      = mmio::read32(bar0_va + NV_USABLE_FB_SIZE_IN_MB);
        wpr2_lo = mmio::read32(bar0_va + NV_PFB_PRI_MMU_WPR2_ADDR_LO);
        wpr2_hi = mmio::read32(bar0_va + NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    });
    const uint64_t fbSize = static_cast<uint64_t>(mb) << 20;

    // ---- FB layout, top-down (kgspCalculateFbLayout_TU102, GA102) ----
    const uint64_t vgaWorkspaceOffset  = fbSize - VBIOS_WORKSPACE_SIZE;
    const uint64_t vgaWorkspaceSize    = fbSize - vgaWorkspaceOffset;
    const uint64_t vbiosReservedOffset = vgaWorkspaceOffset; // no MMU-lock on this GA102
    const uint64_t sizeOfRadix3Elf     = radix3.image.size;  // == pGspFw->imageSize
    const uint64_t gspFwWprEnd         = align_down(vbiosReservedOffset, FB_WPR_ALIGNMENT); // margin=0
    const uint64_t frtsSize            = FRTS_REGION_SIZE;
    const uint64_t frtsOffset          = gspFwWprEnd - frtsSize;
    const uint64_t sizeOfBootloader    = boot.image_size;
    const uint64_t bootBinOffset       = align_down(frtsOffset - sizeOfBootloader, 0x1000);
    const uint64_t gspFwOffset         = align_down(bootBinOffset - sizeOfRadix3Elf, 0x10000);
    const uint64_t wprHeapSize         = calc_fw_heap_size(fbSize, gspFwOffset);
    const uint64_t gspFwHeapOffset     = align_down(gspFwOffset - wprHeapSize, 0x100000);
    const uint64_t gspFwHeapSize       = align_down(gspFwOffset - gspFwHeapOffset, 0x100000);
    const uint64_t gspFwWprStart       = align_down(gspFwHeapOffset - sizeof(GspFwWprMeta), 0x100000);
    const uint64_t nonWprHeapSize      = 0x100000;           // kgspGetNonWprHeapSize GA102 = 1 MiB
    const uint64_t nonWprHeapOffset    = align_down(gspFwWprStart - nonWprHeapSize, 0x100000);
    const uint64_t gspFwRsvdStart      = nonWprHeapOffset;

    // ---- cross-check the recomputed frtsOffset against the Stage-5b carve ----
    const uint64_t carvedFrts = static_cast<uint64_t>(wpr2_lo >> 4) << 12;
    if (frtsOffset != carvedFrts) {
        log::error("nvidia: wprmeta: frtsOffset 0x%lx != carved WPR2_LO 0x%lx (wpr2_lo=0x%08x hi=0x%08x)",
                   static_cast<unsigned long>(frtsOffset), static_cast<unsigned long>(carvedFrts),
                   wpr2_lo, wpr2_hi);
        return ERR_WPRMETA_LAYOUT;
    }

    // ---- allocate + populate the 256-byte WprMeta ----
    if (dma_alloc(sizeof(GspFwWprMeta), /*uncached=*/true, out.buf) != MEM_OK) {
        log::error("nvidia: wprmeta: 256B alloc failed");
        return ERR_WPRMETA_MEM;
    }
    GspFwWprMeta* m = reinterpret_cast<GspFwWprMeta*>(out.buf.cpu_va);
    for (size_t i = 0; i < sizeof(GspFwWprMeta) / sizeof(uint64_t); i++) {
        reinterpret_cast<uint64_t*>(m)[i] = 0;
    }
    m->magic    = GSP_FW_WPR_META_MAGIC;
    m->revision = GSP_FW_WPR_META_REVISION;
    m->sysmemAddrOfRadix3Elf  = radix3.root_phys;
    m->sizeOfRadix3Elf        = sizeOfRadix3Elf;
    m->sysmemAddrOfBootloader = boot.image.phys;
    m->sizeOfBootloader       = sizeOfBootloader;
    m->bootloaderCodeOffset     = boot.desc.monitor_code_offset;
    m->bootloaderDataOffset     = boot.desc.monitor_data_offset;
    m->bootloaderManifestOffset = boot.desc.manifest_offset;
    // sysmemAddrOfSignature / sizeOfSignature: set at the SEC2 Booter stage.
    m->gspFwRsvdStart     = gspFwRsvdStart;
    m->nonWprHeapOffset   = nonWprHeapOffset;
    m->nonWprHeapSize     = nonWprHeapSize;
    m->gspFwWprStart      = gspFwWprStart;
    m->gspFwHeapOffset    = gspFwHeapOffset;
    m->gspFwHeapSize      = gspFwHeapSize;
    m->gspFwOffset        = gspFwOffset;
    m->bootBinOffset      = bootBinOffset;
    m->frtsOffset         = frtsOffset;
    m->frtsSize           = frtsSize;
    m->gspFwWprEnd        = gspFwWprEnd;
    m->fbSize             = fbSize;
    m->vgaWorkspaceOffset = vgaWorkspaceOffset;
    m->vgaWorkspaceSize   = vgaWorkspaceSize;
    m->bootCount = 0;
    m->verified  = 0;
    barrier::dma_full();
    out.meta = m;

    // ---- log the full layout (mirrors the open driver's layout dump) ----
    log::info("nvidia: wprmeta: fbSize=0x%lx (%u MiB), struct @ phys=0x%lx",
              static_cast<unsigned long>(fbSize), mb, static_cast<unsigned long>(out.buf.phys));
    log::info("nvidia: wprmeta: sysRadix3=0x%lx sz=0x%lx | sysBootldr=0x%lx sz=0x%lx",
              static_cast<unsigned long>(m->sysmemAddrOfRadix3Elf),
              static_cast<unsigned long>(m->sizeOfRadix3Elf),
              static_cast<unsigned long>(m->sysmemAddrOfBootloader),
              static_cast<unsigned long>(m->sizeOfBootloader));
    log::info("nvidia: wprmeta: blCode=0x%lx blData=0x%lx blManifest=0x%lx",
              static_cast<unsigned long>(m->bootloaderCodeOffset),
              static_cast<unsigned long>(m->bootloaderDataOffset),
              static_cast<unsigned long>(m->bootloaderManifestOffset));
    log::info("nvidia: wprmeta: nonWprHeap=0x%lx sz=0x%lx | gspFwWprStart=0x%lx",
              static_cast<unsigned long>(nonWprHeapOffset), static_cast<unsigned long>(nonWprHeapSize),
              static_cast<unsigned long>(gspFwWprStart));
    log::info("nvidia: wprmeta: gspFwHeap=0x%lx sz=0x%lx (%lu MiB) | gspFwOffset(ELF)=0x%lx",
              static_cast<unsigned long>(gspFwHeapOffset), static_cast<unsigned long>(gspFwHeapSize),
              static_cast<unsigned long>(gspFwHeapSize >> 20), static_cast<unsigned long>(gspFwOffset));
    log::info("nvidia: wprmeta: bootBin=0x%lx frts=0x%lx frtsSize=0x%lx gspFwWprEnd=0x%lx",
              static_cast<unsigned long>(bootBinOffset), static_cast<unsigned long>(frtsOffset),
              static_cast<unsigned long>(frtsSize), static_cast<unsigned long>(gspFwWprEnd));
    log::info("nvidia: wprmeta: frtsOffset == carved WPR2_LO (0x%lx) - layout consistent",
              static_cast<unsigned long>(carvedFrts));
    return WPRMETA_OK;
}

void gsp_free_wprmeta(gsp_wprmeta& out) {
    dma_free(out.buf);
    out.buf = dma_buffer{};
    out.meta = nullptr;
}

} // namespace nvidia
