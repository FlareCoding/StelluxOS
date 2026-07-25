#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_DISPLAY_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_DISPLAY_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_rpc.h"

// Stage 12: display object tree + connector detection. Builds the RM object tree
// (client -> device -> subdevice -> display_common) over GSP_RM_ALLOC, then runs
// NV0073 controls to enumerate heads + connected displays + read EDID. Faithful
// to the open driver's nvkms object model (spec F06.5-F06.8, F08.3-F08.4).
namespace nvidia {

constexpr int32_t DISP_OK         = 0;
constexpr int32_t ERR_DISP_ALLOC  = -140;
constexpr int32_t ERR_DISP_CTRL   = -141;

/**
 * @brief Allocate the display object tree and detect connected displays (+EDID).
 * @param rpc     the live GSP RPC plane (post-INIT_DONE).
 * @param ctx     sequencer ctx (for the recv/dispatch path).
 * @param bar0_va mapped BAR0 (for the command-queue doorbell).
 * @return DISP_OK on success; negative ERR_DISP_* otherwise.
 */
int32_t gsp_display_init(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_DISPLAY_H
