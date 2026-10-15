// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

// The base of the GSP's falcon registers, and of its second register block, which holds its RISC-V core's registers
constexpr uint32_t GSP_FALCON_BASE  = 0x110000;
constexpr uint32_t GSP_FALCON2_BASE = 0x111000;

// The GSP's engine register, whose bit 0 holds the GSP in reset while set, and the reads of it NVIDIA's driver makes
// after each write so the reset signal propagates
constexpr uint32_t GSP_FALCON_ENGINE       = 0x1103C0;
constexpr uint32_t ENGINE_RESET            = 0x1;
constexpr uint32_t RESET_PROPAGATION_READS = 10;

// The falcon's hardware configuration register: bit 10 set when the GSP has a RISC-V core, bit 12 set while the
// falcon clears its memories after a reset, bit 31 set once the falcon is ready for a reset
constexpr uint32_t GSP_FALCON_HWCFG2 = GSP_FALCON_BASE + 0x0F4;
constexpr uint32_t HAS_RISCV_CORE    = 0x400;
constexpr uint32_t CLEARING_MEMORIES = 0x1000;
constexpr uint32_t READY_FOR_RESET   = 0x80000000;

// The falcon register NVIDIA's driver writes the chip's `PMC_BOOT_0` value to as a reset ends
constexpr uint32_t GSP_FALCON_RM = GSP_FALCON_BASE + 0x084;

// The RISC-V core's boot control register: bit 4 set while the RISC-V core is selected over the falcon core, bit 0
// set once a selection has taken effect, and the value NVIDIA's driver writes to select the falcon core
constexpr uint32_t GSP_RISCV_BCR_CTRL  = GSP_FALCON2_BASE + 0x668;
constexpr uint32_t RISCV_CORE_SELECTED = 0x10;
constexpr uint32_t CORE_SELECT_VALID   = 0x1;
constexpr uint32_t SELECT_FALCON_CORE  = 0x0;

constexpr uint64_t NS_PER_MS = 1000000;

// Spins until the `bits` of the register at `offset` read as `value` or `timeout_ns` passes, reading it at least
// once, and returns whether they did
static bool wait_for_bits(const registers& regs, uint32_t offset, uint32_t bits, uint32_t value, uint64_t timeout_ns) {
    uint64_t deadline = clock::now_ns() + timeout_ns;

    while (true) {
        if ((regs.read(offset) & bits) == value) {
            return true;
        }

        if (clock::now_ns() > deadline) {
            return false;
        }

        cpu::relax();
    }
}

static void let_reset_propagate(const registers& regs) {
    for (uint32_t i = 0; i < RESET_PROPAGATION_READS; i++) {
        (void)regs.read(GSP_FALCON_ENGINE);
    }
}

static void pulse_engine_reset(registers& regs) {
    uint32_t engine = regs.read(GSP_FALCON_ENGINE);
    regs.write(GSP_FALCON_ENGINE, engine | ENGINE_RESET);
    let_reset_propagate(regs);

    engine = regs.read(GSP_FALCON_ENGINE);
    regs.write(GSP_FALCON_ENGINE, engine & ~ENGINE_RESET);
    let_reset_propagate(regs);
}

static int32_t wait_for_cleared_memories(const registers& regs, uint64_t timeout_ns) {
    if (!wait_for_bits(regs, GSP_FALCON_HWCFG2, CLEARING_MEMORIES, 0, timeout_ns)) {
        log::error(
            "nvidia: the GSP's falcon did not finish clearing its memories within %lu ms",
            timeout_ns / NS_PER_MS
        );
        return ERR_TIMEOUT;
    }

    return OK;
}

static int32_t select_falcon_core(registers& regs, uint64_t timeout_ns) {
    bool has_riscv_core = (regs.read(GSP_FALCON_HWCFG2) & HAS_RISCV_CORE) != 0;
    bool is_riscv_selected = has_riscv_core && (regs.read(GSP_RISCV_BCR_CTRL) & RISCV_CORE_SELECTED) != 0;
    if (!is_riscv_selected) {
        return OK;
    }

    regs.write(GSP_RISCV_BCR_CTRL, SELECT_FALCON_CORE);
    if (!wait_for_bits(regs, GSP_RISCV_BCR_CTRL, CORE_SELECT_VALID, CORE_SELECT_VALID, timeout_ns)) {
        log::error(
            "nvidia: the GSP did not switch from its RISC-V core to its falcon core within %lu ms",
            timeout_ns / NS_PER_MS
        );
        return ERR_TIMEOUT;
    }

    return OK;
}

static int32_t reset_in_full(registers& regs, uint64_t ready_timeout_ns, uint64_t timeout_ns) {
    // The ready bit may never be set, so the reset goes on without it, as in NVIDIA's driver
    (void)wait_for_bits(regs, GSP_FALCON_HWCFG2, READY_FOR_RESET, READY_FOR_RESET, ready_timeout_ns);
    pulse_engine_reset(regs);

    int32_t rc = wait_for_cleared_memories(regs, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    return select_falcon_core(regs, timeout_ns);
}

int32_t reset_gsp_falcon(registers& regs, uint32_t boot0, uint64_t ready_timeout_ns, uint64_t timeout_ns) {
    int32_t rc = reset_in_full(regs, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = reset_in_full(regs, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = select_falcon_core(regs, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = wait_for_cleared_memories(regs, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    regs.write(GSP_FALCON_RM, boot0);

    return OK;
}

} // namespace drivers::nvidia
