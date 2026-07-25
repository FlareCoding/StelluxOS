#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_WPRMETA_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_WPRMETA_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_mem.h"
#include "drivers/gpu/nvidia/nv_radix3.h"
#include "drivers/gpu/nvidia/nv_bootimg.h"

// GspFwWprMeta - the Booter<->GSP-RM handoff block. CPU-RM computes the FB
// layout, fills this 256-byte struct, and (later) DMAs it to the top of WPR2;
// the Booter verifies + locks it. Exact mirror of open-gpu-kernel-modules
// arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h. Layout computed per
// kgspCalculateFbLayout_TU102 (kernel_gsp_tu102.c:509-666). Host-side only here.
namespace nvidia {

constexpr uint64_t GSP_FW_WPR_META_MAGIC    = 0xdc3aae21371a60b3ull;
constexpr uint64_t GSP_FW_WPR_META_REVISION = 1;
constexpr uint64_t GSP_FW_WPR_META_VERIFIED = 0xa0a0a0a0a0a0a0a0ull;

constexpr int32_t WPRMETA_OK         = 0;
constexpr int32_t ERR_WPRMETA_MEM    = -90;
constexpr int32_t ERR_WPRMETA_LAYOUT = -91; // computed frtsOffset != carved WPR2

// 256-byte WPR metadata (see header above). Field order/sizes are ABI with the
// GSP firmware - do not reorder. Unions kept identical to the source so offsets
// match byte-for-byte.
struct GspFwWprMeta {
    uint64_t magic;
    uint64_t revision;
    uint64_t sysmemAddrOfRadix3Elf;
    uint64_t sizeOfRadix3Elf;
    uint64_t sysmemAddrOfBootloader;
    uint64_t sizeOfBootloader;
    uint64_t bootloaderCodeOffset;
    uint64_t bootloaderDataOffset;
    uint64_t bootloaderManifestOffset;
    union {
        struct {
            uint64_t sysmemAddrOfSignature;
            uint64_t sizeOfSignature;
        };
        struct {
            uint32_t gspFwHeapFreeListWprOffset;
            uint32_t unused0;
            uint64_t unused1;
        };
    };
    uint64_t gspFwRsvdStart;
    uint64_t nonWprHeapOffset;
    uint64_t nonWprHeapSize;
    uint64_t gspFwWprStart;
    uint64_t gspFwHeapOffset;
    uint64_t gspFwHeapSize;
    uint64_t gspFwOffset;
    uint64_t bootBinOffset;
    uint64_t frtsOffset;
    uint64_t frtsSize;
    uint64_t gspFwWprEnd;
    uint64_t fbSize;
    uint64_t vgaWorkspaceOffset;
    uint64_t vgaWorkspaceSize;
    uint64_t bootCount;
    union {
        struct {
            uint64_t partitionRpcAddr;
            uint16_t partitionRpcRequestOffset;
            uint16_t partitionRpcReplyOffset;
            uint32_t elfCodeOffset;
            uint32_t elfDataOffset;
            uint32_t elfCodeSize;
            uint32_t elfDataSize;
            uint32_t lsUcodeVersion;
        };
        struct {
            uint32_t partitionRpcPadding[4];
            uint64_t sysmemAddrOfCrashReportQueue;
            uint32_t sizeOfCrashReportQueue;
            uint32_t lsUcodeVersionPadding[1];
        };
    };
    uint8_t  gspFwHeapVfPartitionCount;
    uint8_t  flags;
    uint8_t  padding[6];
    uint64_t verified;
};
static_assert(sizeof(GspFwWprMeta) == 256, "GspFwWprMeta must be exactly 256 bytes");

struct gsp_wprmeta {
    dma_buffer    buf;  // 256-byte WprMeta in sysmem (Booter reads it by phys)
    GspFwWprMeta* meta; // CPU view == (GspFwWprMeta*)buf.cpu_va
};

/**
 * @brief Compute the GSP FB layout and populate the 256-byte GspFwWprMeta.
 * Mirrors kgspCalculateFbLayout_TU102 for GA102 (no MMU-lock, wprEndMargin=0,
 * no scrubber ucode). Reads usable FB size + the carved WPR2 registers from
 * BAR0 and asserts the recomputed frtsOffset matches the Stage-5b carve. The
 * signature fields are left zero here (filled in at the SEC2 Booter stage).
 * @return WPRMETA_OK on success; negative ERR_WPRMETA_* otherwise. Heavily logged.
 */
int32_t gsp_build_wprmeta(uintptr_t bar0_va, const gsp_radix3& radix3,
                          const gsp_boot_ucode& boot, gsp_wprmeta& out);

/** @brief Release the WprMeta buffer. */
void gsp_free_wprmeta(gsp_wprmeta& out);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_WPRMETA_H
