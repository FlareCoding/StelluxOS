#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_SEQ_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_SEQ_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_falcon.h"

// GSP CPU sequencer (Stage 10). During GSP-RM boot the GSP posts a
// run_cpu_sequencer RPC whose payload is a buffer of opcodes the host must
// execute (register pokes + GSP/SEC2 core control) to bring GSP-RM the rest of
// the way up. Faithful port of kgspExecuteSequencerBuffer_IMPL (kernel_gsp.c)
// + kgspExecuteSequencerCommand_GA102 (CORE_RESUME).
namespace nvidia {

constexpr int32_t SEQ_OK         = 0;
constexpr int32_t ERR_SEQ_OPCODE = -120;
constexpr int32_t ERR_SEQ_BOUNDS = -121;
constexpr int32_t ERR_SEQ_POLL   = -122;
constexpr int32_t ERR_SEQ_CORE   = -123;

// Context the sequencer needs: BAR0 (for REG ops), the GSP + SEC2 Falcons, the
// chip id (CORE_RESET) and libos-args phys (CORE_RESUME programs GSP MAILBOX).
struct gsp_seq_ctx {
    uintptr_t bar0_va;
    falcon    gsp;
    falcon    sec2;
    uint32_t  chip_id;
    uint64_t  libos_args_phys;
};

/**
 * @brief Execute a GSP-supplied sequencer command buffer (cmd[0..cmd_count)).
 * reg_save is the 8-entry REG_STORE save area. Heavily logged (CORE_* ops +
 * summary + errors). @return SEQ_OK on success; negative ERR_SEQ_* otherwise.
 */
int32_t gsp_run_sequencer(const gsp_seq_ctx& ctx, const uint32_t* cmd,
                          uint32_t cmd_count, uint32_t* reg_save);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_SEQ_H
