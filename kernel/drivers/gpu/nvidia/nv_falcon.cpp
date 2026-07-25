#include "drivers/gpu/nvidia/nv_falcon.h"
#include "drivers/gpu/nvidia/nv_regs.h"
#include "hw/mmio.h"
#include "hw/cpu.h"
#include "hw/delay.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace nvidia {

// Register accessors (assume the caller is already elevated). All apertures are
// within the mapped BAR0; offsets are relative to the respective base.
static inline void reg_wr(const falcon& f, uint32_t off, uint32_t v) {
    mmio::write32(f.bar0_va + f.reg_base + off, v);
}
static inline uint32_t reg_rd(const falcon& f, uint32_t off) {
    return mmio::read32(f.bar0_va + f.reg_base + off);
}
static inline void riscv_wr(const falcon& f, uint32_t off, uint32_t v) {
    mmio::write32(f.bar0_va + f.riscv_base + off, v);
}
static inline uint32_t riscv_rd(const falcon& f, uint32_t off) {
    return mmio::read32(f.bar0_va + f.riscv_base + off);
}
static inline void fbif_wr(const falcon& f, uint32_t off, uint32_t v) {
    mmio::write32(f.bar0_va + f.fbif_base + off, v);
}
static inline uint32_t fbif_rd(const falcon& f, uint32_t off) {
    return mmio::read32(f.bar0_va + f.fbif_base + off);
}

constexpr uint64_t DMA_TIMEOUT_NS  = 1000000000ull; // 1 s per 256-byte block poll
constexpr uint64_t HALT_TIMEOUT_NS = 4000000000ull; // 4 s for FWSEC to run+halt

// Poll until (reg & mask) is set / cleared, with a 1s timeout. Already elevated.
static void poll_set(const falcon& f, uint32_t off, uint32_t mask) {
    const uint64_t s = clock::now_ns();
    while ((reg_rd(f, off) & mask) == 0) {
        if (clock::now_ns() - s > DMA_TIMEOUT_NS) break;
        delay::us(10);
    }
}
static void poll_clear(const falcon& f, uint32_t off, uint32_t mask) {
    const uint64_t s = clock::now_ns();
    while (reg_rd(f, off) & mask) {
        if (clock::now_ns() - s > DMA_TIMEOUT_NS) break;
        delay::us(10);
    }
}
// Switch the core to FALCON mode (BCR CORE_SELECT=FALCON) and wait for VALID.
static void switch_to_falcon(const falcon& f) {
    riscv_wr(f, NV_PRISCV_RISCV_BCR_CTRL, BCR_CTRL_CORE_SELECT_FALCON);
    const uint64_t s = clock::now_ns();
    while ((riscv_rd(f, NV_PRISCV_RISCV_BCR_CTRL) & BCR_CTRL_VALID) == 0) {
        if (clock::now_ns() - s > DMA_TIMEOUT_NS) break;
        delay::us(10);
    }
}

void falcon_reset(const falcon& f, uint32_t chip_id) {
    RUN_ELEVATED({
        // kflcnReset = enable(FALSE)+enable(TRUE); each is a secure reset:
        //   PreResetWait (RESET_READY) -> engine reset -> wait scrub -> switch-to-Falcon.
        for (int i = 0; i < 2; i++) {
            poll_set(f, NV_PFALCON_FALCON_HWCFG2, FALCON_HWCFG2_RESET_READY);
            reg_wr(f, NV_PFALCON_FALCON_ENGINE, 1);
            reg_wr(f, NV_PFALCON_FALCON_ENGINE, 0);
            poll_clear(f, NV_PFALCON_FALCON_HWCFG2, FALCON_HWCFG2_MEM_SCRUBBING);
            switch_to_falcon(f);
        }
        // enable(TRUE) tail: ensure Falcon mode, wait scrub, hand over chip id.
        switch_to_falcon(f);
        poll_clear(f, NV_PFALCON_FALCON_HWCFG2, FALCON_HWCFG2_MEM_SCRUBBING);
        reg_wr(f, NV_PFALCON_FALCON_RM, chip_id);

        const uint32_t hwcfg2 = reg_rd(f, NV_PFALCON_FALCON_HWCFG2);
        const uint32_t bcr    = riscv_rd(f, NV_PRISCV_RISCV_BCR_CTRL);
        log::info("nvidia: falcon: reset done HWCFG2=0x%08x (riscv=%u scrub=%u rdy=%u) BCR=0x%08x (coreSel=%u valid=%u)",
                  hwcfg2, (hwcfg2 & FALCON_HWCFG2_RISCV) ? 1u : 0u,
                  (hwcfg2 & FALCON_HWCFG2_MEM_SCRUBBING) ? 1u : 0u,
                  (hwcfg2 & FALCON_HWCFG2_RESET_READY) ? 1u : 0u,
                  bcr, (bcr & BCR_CTRL_CORE_SELECT) ? 1u : 0u, (bcr & BCR_CTRL_VALID) ? 1u : 0u);
    });
}

// One DMA segment (IMEM or DMEM) via the 256-byte transfer loop. Already elevated.
static int32_t dma_transfer(const falcon& f, uint32_t dest, uint32_t memOff,
                            uint64_t srcPhys, uint32_t size, uint32_t cmd) {
    reg_wr(f, NV_PFALCON_FALCON_DMATRFBASE,
           static_cast<uint32_t>((srcPhys >> 8) & 0xFFFFFFFFull));
    reg_wr(f, NV_PFALCON_FALCON_DMATRFBASE1,
           static_cast<uint32_t>((srcPhys >> 8) >> 32) & 0x1FF);

    uint32_t xfered = 0;
    while (xfered < size) {
        reg_wr(f, NV_PFALCON_FALCON_DMATRFMOFFS, dest & 0xFFFFFF);
        reg_wr(f, NV_PFALCON_FALCON_DMATRFFBOFFS, memOff);
        reg_wr(f, NV_PFALCON_FALCON_DMATRFCMD, cmd);

        const uint64_t start = clock::now_ns();
        while ((reg_rd(f, NV_PFALCON_FALCON_DMATRFCMD) & FALCON_DMATRFCMD_IDLE) == 0) {
            if (clock::now_ns() - start > DMA_TIMEOUT_NS) {
                log::error("nvidia: falcon: DMA timeout at xfered=%u/%u", xfered, size);
                return ERR_FLCN_DMA_TIMEOUT;
            }
            cpu::relax();
        }

        xfered += FLCN_BLK_ALIGNMENT;
        dest   += FLCN_BLK_ALIGNMENT;
        memOff += FLCN_BLK_ALIGNMENT;
    }
    return FLCN_OK;
}

int32_t falcon_hs_execute(const falcon& f, const flcn_hs_ucode& u,
                          uint32_t* mailbox0, uint32_t* mailbox1) {
    int32_t rc = FLCN_OK;
    RUN_ELEVATED({
        // Disable context requirement: allow physical DMA without a bound ctx.
        const uint32_t ctl = fbif_rd(f, NV_PFALCON_FBIF_CTL);
        fbif_wr(f, NV_PFALCON_FBIF_CTL, ctl | FBIF_CTL_ALLOW_PHYS_NO_CTX);
        reg_wr(f, NV_PFALCON_FALCON_DMACTL, 0);

        // Point the DMA engine at coherent sysmem, physical addressing.
        uint32_t tc = fbif_rd(f, NV_PFALCON_FBIF_TRANSCFG);
        tc = (tc & ~FBIF_TRANSCFG_TARGET_MASK) |
             FBIF_TRANSCFG_TARGET_COHERENT_SYSMEM | FBIF_TRANSCFG_MEM_TYPE_PHYSICAL;
        fbif_wr(f, NV_PFALCON_FBIF_TRANSCFG, tc);

        // DMA the code into IMEM (secure).
        const uint32_t imemCmd =
            FALCON_DMATRFCMD_SIZE_256B | FALCON_DMATRFCMD_IMEM | FALCON_DMATRFCMD_SEC1;
        rc = dma_transfer(f, u.imem_pa, u.imem_va, u.ucode_phys, u.imem_size, imemCmd);

        if (rc == FLCN_OK) {
            // DMA the data into DMEM (non-secure).
            const uint32_t dmemCmd = FALCON_DMATRFCMD_SIZE_256B;
            rc = dma_transfer(f, u.dmem_pa, 0, u.ucode_phys + u.data_offset,
                              u.dmem_size, dmemCmd);
        }

        if (rc == FLCN_OK) {
            // Program BROM/PKC signature validation.
            riscv_wr(f, NV_PFALCON2_FALCON_BROM_PARAADDR0, u.hs_sig_dmem_addr);
            riscv_wr(f, NV_PFALCON2_FALCON_BROM_ENGIDMASK, u.engine_id_mask);
            riscv_wr(f, NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID, u.ucode_id & 0xFF);
            riscv_wr(f, NV_PFALCON2_FALCON_MOD_SEL, NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K);

            // Boot vector.
            reg_wr(f, NV_PFALCON_FALCON_BOOTVEC, u.imem_va);

            // Optional mailbox inputs (Booter passes the WprMeta phys here).
            if (mailbox0 != nullptr) reg_wr(f, NV_PFALCON_FALCON_MAILBOX0, *mailbox0);
            if (mailbox1 != nullptr) reg_wr(f, NV_PFALCON_FALCON_MAILBOX1, *mailbox1);

            // Diagnostics: core/CPU state right before we start the secure CPU.
            const uint32_t cpuctl0 = reg_rd(f, NV_PFALCON_FALCON_CPUCTL);
            const uint32_t bcr0    = riscv_rd(f, NV_PRISCV_RISCV_BCR_CTRL);
            log::info("nvidia: falcon: pre-start CPUCTL=0x%08x (aliasEn=%u) BCR=0x%08x (coreSel=%u valid=%u)",
                      cpuctl0, (cpuctl0 & FALCON_CPUCTL_ALIAS_EN) ? 1u : 0u,
                      bcr0, (bcr0 & BCR_CTRL_CORE_SELECT) ? 1u : 0u, (bcr0 & BCR_CTRL_VALID) ? 1u : 0u);

            // Start the CPU (via CPUCTL_ALIAS if aliasing is enabled, else CPUCTL).
            if (cpuctl0 & FALCON_CPUCTL_ALIAS_EN) {
                reg_wr(f, NV_PFALCON_FALCON_CPUCTL_ALIAS, FALCON_CPUCTL_ALIAS_STARTCPU);
            } else {
                reg_wr(f, NV_PFALCON_FALCON_CPUCTL, FALCON_CPUCTL_STARTCPU);
            }

            // Wait for the Falcon to halt.
            rc = ERR_FLCN_HALT_TIMEOUT;
            const uint64_t start = clock::now_ns();
            while (clock::now_ns() - start < HALT_TIMEOUT_NS) {
                if (reg_rd(f, NV_PFALCON_FALCON_CPUCTL) & FALCON_CPUCTL_HALTED) {
                    rc = FLCN_OK;
                    break;
                }
                delay::us(50);
            }

            // Diagnostics: where did it end up? (mailboxes carry HS/BROM error codes.)
            const uint32_t cpuctl1 = reg_rd(f, NV_PFALCON_FALCON_CPUCTL);
            const uint32_t mb0     = reg_rd(f, NV_PFALCON_FALCON_MAILBOX0);
            const uint32_t mb1     = reg_rd(f, NV_PFALCON_FALCON_MAILBOX1);
            const uint32_t rcpuctl = riscv_rd(f, NV_PRISCV_RISCV_CPUCTL);
            log::info("nvidia: falcon: post CPUCTL=0x%08x (halted=%u) MAILBOX0=0x%08x MAILBOX1=0x%08x RISCV_CPUCTL=0x%08x (active=%u) rc=%d",
                      cpuctl1, (cpuctl1 & FALCON_CPUCTL_HALTED) ? 1u : 0u, mb0, mb1,
                      rcpuctl, (rcpuctl & PRISCV_RISCV_CPUCTL_ACTIVE_STAT) ? 1u : 0u, rc);

            // Mailbox outputs (Booter result code lands in MAILBOX0; 0 == success).
            if (mailbox0 != nullptr) *mailbox0 = mb0;
            if (mailbox1 != nullptr) *mailbox1 = mb1;
        }
    });
    return rc;
}

void falcon_reset_into_riscv(const falcon& f) {
    RUN_ELEVATED({
        // _kgspResetIntoRiscv: PreResetWait -> single engine-reset pulse (with
        // reg-read propagation delays) -> wait scrub -> BCR = RISCV|VALID|BRFETCH.
        poll_set(f, NV_PFALCON_FALCON_HWCFG2, FALCON_HWCFG2_RESET_READY);
        reg_wr(f, NV_PFALCON_FALCON_ENGINE, 1);
        for (int i = 0; i < 10; i++) (void)reg_rd(f, NV_PFALCON_FALCON_ENGINE);
        reg_wr(f, NV_PFALCON_FALCON_ENGINE, 0);
        for (int i = 0; i < 10; i++) (void)reg_rd(f, NV_PFALCON_FALCON_ENGINE);
        poll_clear(f, NV_PFALCON_FALCON_HWCFG2, FALCON_HWCFG2_MEM_SCRUBBING);
        riscv_wr(f, NV_PRISCV_RISCV_BCR_CTRL, BCR_CTRL_RISCV_BOOT);

        const uint32_t hwcfg2 = reg_rd(f, NV_PFALCON_FALCON_HWCFG2);
        const uint32_t bcr    = riscv_rd(f, NV_PRISCV_RISCV_BCR_CTRL);
        log::info("nvidia: falcon: reset-into-RISCV HWCFG2=0x%08x (riscv=%u scrub=%u) BCR=0x%08x (coreSel=%u valid=%u brfetch=%u)",
                  hwcfg2, (hwcfg2 & FALCON_HWCFG2_RISCV) ? 1u : 0u,
                  (hwcfg2 & FALCON_HWCFG2_MEM_SCRUBBING) ? 1u : 0u, bcr,
                  (bcr & BCR_CTRL_CORE_SELECT) ? 1u : 0u, (bcr & BCR_CTRL_VALID) ? 1u : 0u,
                  (bcr & BCR_CTRL_BRFETCH) ? 1u : 0u);
    });
}

bool falcon_is_riscv_active(const falcon& f) {
    bool active = false;
    RUN_ELEVATED({
        active = (riscv_rd(f, NV_PRISCV_RISCV_CPUCTL) & PRISCV_RISCV_CPUCTL_ACTIVE_STAT) != 0;
    });
    return active;
}

void falcon_disable_ctx_req(const falcon& f) {
    RUN_ELEVATED({
        const uint32_t ctl = fbif_rd(f, NV_PFALCON_FBIF_CTL);
        fbif_wr(f, NV_PFALCON_FBIF_CTL, ctl | FBIF_CTL_ALLOW_PHYS_NO_CTX);
        reg_wr(f, NV_PFALCON_FALCON_DMACTL, 0);
    });
}

void falcon_start_cpu(const falcon& f) {
    RUN_ELEVATED({
        const uint32_t cpuctl = reg_rd(f, NV_PFALCON_FALCON_CPUCTL);
        if (cpuctl & FALCON_CPUCTL_ALIAS_EN) {
            reg_wr(f, NV_PFALCON_FALCON_CPUCTL_ALIAS, FALCON_CPUCTL_ALIAS_STARTCPU);
        } else {
            reg_wr(f, NV_PFALCON_FALCON_CPUCTL, FALCON_CPUCTL_STARTCPU);
        }
    });
}

int32_t falcon_wait_for_halt(const falcon& f) {
    int32_t rc = ERR_FLCN_HALT_TIMEOUT;
    RUN_ELEVATED({
        const uint64_t start = clock::now_ns();
        while (clock::now_ns() - start < HALT_TIMEOUT_NS) {
            if (reg_rd(f, NV_PFALCON_FALCON_CPUCTL) & FALCON_CPUCTL_HALTED) { rc = FLCN_OK; break; }
            delay::us(50);
        }
    });
    return rc;
}

} // namespace nvidia
