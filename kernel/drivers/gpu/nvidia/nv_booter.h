#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_BOOTER_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_BOOTER_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_mem.h"
#include "drivers/gpu/nvidia/nv_firmware.h"

// SEC2 Booter Load (Stage 9). The Booter authenticates the GSP-RM image into
// WPR2 and starts the GSP RISC-V core. It is itself a HS Falcon ucode (BOOT_FROM_HS
// on GA102), so it runs through the same nv_falcon engine as FWSEC, but on the
// SEC2 Falcon, with the WprMeta phys handed in via SEC2 MAILBOX0/1.
// Source: kgspExecuteBooterLoad_TU102 / s_allocateUcodeFromBinArchive (booter.c).
namespace nvidia {

constexpr int32_t BOOTER_OK          = 0;
constexpr int32_t ERR_BOOTER_IO      = -110; // missing/short Booter file
constexpr int32_t ERR_BOOTER_SIZE    = -111;
constexpr int32_t ERR_BOOTER_SIG     = -112; // signature select/size problem
constexpr int32_t ERR_BOOTER_MEM     = -113;
constexpr int32_t ERR_BOOTER_EXEC    = -114; // SEC2 Falcon didn't halt
constexpr int32_t ERR_BOOTER_MAILBOX = -115; // Booter returned non-zero error code

/**
 * @brief Load the GSP-RM signature (.fwsignature_ga10x section of gsp_ga10x.bin)
 * into a CACHED, 256-aligned DMA buffer. Its phys + size go into the WprMeta so
 * the Booter can verify the GSP-RM image.
 */
int32_t gsp_load_signature(const gsp_fw_image& fw, dma_buffer& out);

/**
 * @brief Load the SEC2 Booter Load ucode from the initrd, patch in the
 * fuse-selected signature, and execute it on the SEC2 Falcon with the WprMeta
 * physical address in MAILBOX0/1. On return the Booter has authenticated GSP-RM
 * into WPR2 and started the GSP RISC-V. Self-contained (frees its own buffers).
 * @return BOOTER_OK iff the Falcon halted and MAILBOX0 read back 0.
 */
int32_t booter_load_execute(uintptr_t bar0_va, uint32_t chip_id, uint64_t wprmeta_phys);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_BOOTER_H
