#include "drivers/gpu/nvidia/nv_seq.h"
#include "drivers/gpu/nvidia/nv_regs.h"
#include "hw/mmio.h"
#include "hw/cpu.h"
#include "hw/delay.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace nvidia {

// GSP_SEQ_BUF_OPCODE (rmgspseq.h)
enum {
    SEQ_REG_WRITE = 0,
    SEQ_REG_MODIFY,
    SEQ_REG_POLL,
    SEQ_DELAY_US,
    SEQ_REG_STORE,
    SEQ_CORE_RESET,
    SEQ_CORE_START,
    SEQ_CORE_WAIT_FOR_HALT,
    SEQ_CORE_RESUME,
};

// GSP_SEQUENCER_PAYLOAD_SIZE_DWORDS
static uint32_t payload_dwords(uint32_t op) {
    switch (op) {
        case SEQ_REG_WRITE:  return 2;
        case SEQ_REG_MODIFY: return 3;
        case SEQ_REG_POLL:   return 5;
        case SEQ_DELAY_US:   return 1;
        case SEQ_REG_STORE:  return 2;
        default:             return 0; // CORE_RESET/START/WAIT/RESUME
    }
}

int32_t gsp_run_sequencer(const gsp_seq_ctx& ctx, const uint32_t* cmd,
                          uint32_t cmd_count, uint32_t* reg_save) {
    int32_t rc = SEQ_OK;
    RUN_ELEVATED({
        uint32_t i = 0;
        uint32_t nOps = 0;
        uint32_t nCore = 0;
        while (i < cmd_count) {
            const uint32_t op  = cmd[i++];
            const uint32_t pdw = payload_dwords(op);
            if (i + pdw > cmd_count) {
                log::error("nvidia: seq: truncated op=%u at dw=%u (need %u, have %u)",
                           op, i - 1, pdw, cmd_count - i);
                rc = ERR_SEQ_BOUNDS;
                break;
            }
            const uint32_t* a = &cmd[i];
            bool stop = false;

            // NB: no per-opcode logging here. The first opcode is a time-critical
            // MAILBOX0 handshake poll; a slow serial log line before it (added as a
            // diagnostic in run #18) shifted timing enough to miss the GSP's bit-31
            // window. The golden host runs the sequencer with minimal overhead.

            switch (op) {
                case SEQ_REG_WRITE:
                    mmio::write32(ctx.bar0_va + a[0], a[1]);
                    break;
                case SEQ_REG_MODIFY: {
                    uint32_t v = mmio::read32(ctx.bar0_va + a[0]);
                    v = (v & ~a[1]) | a[2];
                    mmio::write32(ctx.bar0_va + a[0], v);
                    break;
                }
                case SEQ_REG_POLL: {
                    // a[3] is the GSP-provided timeout. RM convention (gpu_timeout.c
                    // timeoutSet:214 + gpu_timeout.h:40): 0 == GPU_TIMEOUT_DEFAULT,
                    // which maps to the platform default (osGetTimeoutParams => ~4 s),
                    // NOT a zero-length timeout. Non-zero values are microseconds
                    // (gpuSetTimeout: timeoutNs = timeoutUs*1000). Treating 0 as a
                    // literal zero was the run #17/#18 bug (instant, race-dependent
                    // timeout on the MAILBOX0 handshake + DMATRFCMD IDLE polls).
                    const uint64_t to_ns = (a[3] == 0)
                        ? 4000000000ull
                        : (static_cast<uint64_t>(a[3]) * 1000ull);
                    const uint64_t s = clock::now_ns();
                    bool ok = false;
                    for (;;) {
                        const uint32_t v = mmio::read32(ctx.bar0_va + a[0]);
                        if ((v & a[1]) == a[2]) { ok = true; break; }
                        if (clock::now_ns() - s > to_ns) {
                            log::error("nvidia: seq: REG_POLL timeout op#%u addr=0x%x mask=0x%x want=0x%x got=0x%x err=0x%x timeoutMs=%u",
                                       nOps, a[0], a[1], a[2], v, a[4], a[3]);
                            break;
                        }
                        cpu::relax(); // tight spin (== osSpinLoop); poll is time-critical
                    }
                    if (!ok) { rc = ERR_SEQ_POLL; stop = true; }
                    break;
                }
                case SEQ_DELAY_US:
                    delay::us(a[0]);
                    break;
                case SEQ_REG_STORE:
                    if (a[1] < 8) reg_save[a[1]] = mmio::read32(ctx.bar0_va + a[0]);
                    break;
                case SEQ_CORE_RESET:
                    log::info("nvidia: seq: CORE_RESET (GSP)");
                    falcon_reset(ctx.gsp, ctx.chip_id);
                    falcon_disable_ctx_req(ctx.gsp);
                    nCore++;
                    break;
                case SEQ_CORE_START:
                    log::info("nvidia: seq: CORE_START (GSP)");
                    falcon_start_cpu(ctx.gsp);
                    nCore++;
                    break;
                case SEQ_CORE_WAIT_FOR_HALT:
                    log::info("nvidia: seq: CORE_WAIT_FOR_HALT (GSP)");
                    if (falcon_wait_for_halt(ctx.gsp) != FLCN_OK) {
                        log::error("nvidia: seq: CORE_WAIT_FOR_HALT timeout");
                        rc = ERR_SEQ_CORE;
                        stop = true;
                    }
                    nCore++;
                    break;
                case SEQ_CORE_RESUME:
                    log::info("nvidia: seq: CORE_RESUME (reset-into-RISC-V + libos MAILBOX + SEC2 start)");
                    falcon_reset_into_riscv(ctx.gsp);
                    mmio::write32(ctx.bar0_va + NV_PGSP_FALCON_BASE + NV_PFALCON_FALCON_MAILBOX0,
                                  static_cast<uint32_t>(ctx.libos_args_phys & 0xFFFFFFFFu));
                    mmio::write32(ctx.bar0_va + NV_PGSP_FALCON_BASE + NV_PFALCON_FALCON_MAILBOX1,
                                  static_cast<uint32_t>(ctx.libos_args_phys >> 32));
                    falcon_start_cpu(ctx.sec2);
                    nCore++;
                    break;
                default:
                    log::error("nvidia: seq: unknown opcode %u at dw=%u", op, i - 1);
                    rc = ERR_SEQ_OPCODE;
                    stop = true;
                    break;
            }

            if (stop) break;
            i += pdw;
            nOps++;
        }
        if (rc == SEQ_OK) {
            log::info("nvidia: seq: executed %u opcodes (%u core ops, %u dwords)", nOps, nCore, cmd_count);
        }
    });
    return rc;
}

} // namespace nvidia
