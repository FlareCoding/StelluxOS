#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_FRTS_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_FRTS_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_vbios.h"
#include "drivers/gpu/nvidia/nv_fwsec.h"

// FWSEC-FRTS: the first GPU writes of the bring-up.
//
// Faithful port of s_executeFwsec_TU102 + kgspExecuteFwsecFrts +
// kgspCalculateFbLayout_TU102 (the FRTS slice). Computes frtsOffset from the FB
// layout, patches the FWSEC ucode DMEM (FRTS command + the fuse-selected RSA-3K
// signature), resets the GSP Falcon, runs FWSEC via the Falcon DMA-load-and-go,
// and verifies that WPR2 was carved.
namespace nvidia {

constexpr int32_t FRTS_OK         = 0;
constexpr int32_t ERR_FRTS_PATCH  = -60; // DMEM interface patch failed
constexpr int32_t ERR_FRTS_SIG    = -61; // signature selection failed
constexpr int32_t ERR_FRTS_EXEC   = -62; // Falcon execution failed/timed out
constexpr int32_t ERR_FRTS_WPR2   = -63; // FWSEC ran but WPR2 not carved

/**
 * @brief Execute FWSEC-FRTS on the GSP Falcon to carve WPR2. THIS WRITES TO THE GPU.
 * @param bar0_va  Mapped BAR0 base.
 * @param chip_id  NV_PMC_BOOT_0 value (written to FALCON_RM during reset).
 * @param vb       Extracted VBIOS (source of the RSA-3K signatures).
 * @param fi       Parsed FWSEC descriptor (offsets, sizes, ucodeId, sig versions).
 * @param ucode    Assembled FWSEC ucode DMA buffer (patched in place here).
 * @return FRTS_OK if WPR2 is carved; negative ERR_FRTS_* otherwise. Heavily logged.
 */
int32_t fwsec_frts_execute(uintptr_t bar0_va, uint32_t chip_id,
                           const vbios_image& vb, const fwsec_info& fi,
                           fwsec_ucode& ucode);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_FRTS_H
