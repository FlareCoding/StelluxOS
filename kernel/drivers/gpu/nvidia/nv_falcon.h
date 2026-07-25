#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_FALCON_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_FALCON_H

#include "common/types.h"

// Generic NVIDIA Falcon "DMA-load-and-go" engine for HS (boot-from-HS) ucode.
//
// Faithful port of the open driver's kgspExecuteHsFalcon_GA102 + s_dmaTransfer_GA102
// + kflcnReset/kflcnEnable (kernel_gsp_falcon_ga102.c, kernel_falcon_tu102.c).
// Parameterized by the three Falcon register bases so the same routine drives the
// GSP Falcon (FWSEC, GSP-RM) and later the SEC2 Falcon (Booter). All register
// access goes through the mapped BAR0; callers run in/elevate to privileged mode.
namespace nvidia {

constexpr int32_t FLCN_OK              = 0;
constexpr int32_t ERR_FLCN_DMA_TIMEOUT  = -50;
constexpr int32_t ERR_FLCN_HALT_TIMEOUT = -51;

// A Falcon engine instance (register apertures within the mapped BAR0).
struct falcon {
    uintptr_t bar0_va;     // mapped BAR0 base
    uint32_t  reg_base;    // Falcon register base (GSP: 0x110000)
    uint32_t  riscv_base;  // RISC-V/BROM register base (GSP: 0x111000)
    uint32_t  fbif_base;   // FBIF register base (GSP: 0x110600)
};

// HS ucode placement for falcon_hs_execute (one contiguous [code|data] image).
struct flcn_hs_ucode {
    uint64_t ucode_phys;       // DMA phys of the contiguous image
    uint32_t imem_pa;          // Falcon IMEM dest base
    uint32_t imem_va;          // IMEM virtual base (also BOOTVEC)
    uint32_t imem_size;        // code bytes
    uint32_t dmem_pa;          // Falcon DMEM dest base
    uint32_t data_offset;      // byte offset of data within the image (== imem_size)
    uint32_t dmem_size;        // data bytes
    uint32_t hs_sig_dmem_addr; // signature DMEM addr (BROM PARAADDR)
    uint32_t engine_id_mask;   // BROM ENGIDMASK
    uint32_t ucode_id;         // BROM CURR_UCODE_ID
};

/**
 * @brief Reset a Falcon: engine reset pulse (x2) + BCR clear + FALCON_RM=chipId0,
 * then wait for memory scrubbing to finish. Matches the boot-trace reset writes.
 */
void falcon_reset(const falcon& f, uint32_t chip_id);

/**
 * @brief Reset a Falcon and switch its core into RISC-V mode (BCR =
 * RISCV|VALID|BRFETCH). Port of _kgspResetIntoRiscv (single engine-reset pulse
 * with propagation delays + scrub wait + kflcnRiscvProgramBcr).
 */
void falcon_reset_into_riscv(const falcon& f);

/** @brief True if the Falcon's RISC-V core reports ACTIVE (RISCV_CPUCTL bit 7).
 * NB: once GSP-RM secures itself, RISCV_CORE_SWITCH_RISCV_STATUS (0x240) becomes
 * priv-locked (reads 0xBADFxxxx), so we use RISCV_CPUCTL (0x388) instead. */
bool falcon_is_riscv_active(const falcon& f);

// Standalone Falcon primitives for the CPU sequencer's CORE_* opcodes.
/** @brief Allow physical DMA with no bound ctx (FBIF_CTL.ALLOW_PHYS_NO_CTX + DMACTL=0). */
void falcon_disable_ctx_req(const falcon& f);
/** @brief Start the Falcon CPU (CPUCTL_ALIAS if ALIAS_EN, else CPUCTL). */
void falcon_start_cpu(const falcon& f);
/** @brief Poll CPUCTL.HALTED with a timeout. @return FLCN_OK or ERR_FLCN_HALT_TIMEOUT. */
int32_t falcon_wait_for_halt(const falcon& f);

/**
 * @brief DMA the HS ucode into IMEM/DMEM, program the BROM/PKC signature regs,
 * set BOOTVEC, optionally write MAILBOX0/1, StartCpu, wait for halt, and
 * optionally read MAILBOX0/1 back (matches kgspExecuteHsFalcon's mailbox in/out;
 * the Booter passes the WprMeta phys in / its result code out).
 * @return FLCN_OK on halt; negative ERR_FLCN_* on timeout.
 */
int32_t falcon_hs_execute(const falcon& f, const flcn_hs_ucode& u,
                          uint32_t* mailbox0 = nullptr, uint32_t* mailbox1 = nullptr);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_FALCON_H
