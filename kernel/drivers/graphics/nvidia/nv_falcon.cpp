#include "drivers/graphics/nvidia/nv_falcon.h"
#include "drivers/graphics/nvidia/nv_gpu.h"
#include "hw/delay.h"
#include "clock/clock.h"
#include "common/logging.h"

namespace nv {

// ============================================================================
// Initialization
// ============================================================================

int32_t falcon::init(nv_gpu* gpu, engine_type type) {
    m_gpu = gpu;
    m_type = type;

    switch (type) {
    case engine_type::GSP:
        m_base = reg::FALCON_GSP_BASE;
        break;
    case engine_type::SEC2:
        m_base = reg::FALCON_SEC2_BASE;
        break;
    }

    log::info("nvidia: falcon: initialized %s at BAR0+0x%06x", name(), m_base);
    return OK;
}

const char* falcon::name() const {
    switch (m_type) {
    case engine_type::GSP:  return "GSP";
    case engine_type::SEC2: return "SEC2";
    default:                return "???";
    }
}

// ============================================================================
// Register Access (relative to falcon base)
// ============================================================================

uint32_t falcon::rd32(uint32_t offset) const {
    return m_gpu->reg_rd32(m_base + offset);
}

void falcon::wr32(uint32_t offset, uint32_t val) {
    m_gpu->reg_wr32(m_base + offset, val);
}

void falcon::mask32(uint32_t offset, uint32_t clear, uint32_t set) {
    uint32_t val = rd32(offset);
    wr32(offset, (val & ~clear) | set);
}

uint32_t falcon::mailbox0() const { return rd32(reg::FALCON_MAILBOX0); }
uint32_t falcon::mailbox1() const { return rd32(reg::FALCON_MAILBOX1); }
void falcon::set_mailbox0(uint32_t val) { wr32(reg::FALCON_MAILBOX0, val); }
void falcon::set_mailbox1(uint32_t val) { wr32(reg::FALCON_MAILBOX1, val); }

// ============================================================================
// Reset Sequence (GA102)
// ============================================================================

int32_t falcon::reset() {
    log::info("nvidia: falcon: resetting %s...", name());

    // Step 1: Disable IRQs
    wr32(reg::FALCON_IRQMODE, 0x00000000);
    wr32(reg::FALCON_IRQMASK, 0xFFFFFFFF);

    // Step 2: Wait for reset ready (may not assert on Ampere — 150µs timeout)
    uint64_t deadline = clock::now_ns() + 150000; // 150µs
    while (!(rd32(reg::FALCON_HWCFG2) & reg::FALCON_HWCFG2_RESET_READY)) {
        if (clock::now_ns() >= deadline) {
            log::info("nvidia: falcon: %s reset_ready not asserted (normal on Ampere)", name());
            break;
        }
        delay::us(1);
    }

    // Step 3: Assert and deassert engine reset
    wr32(reg::FALCON_ENGINE, 0x00000001); // Assert reset
    delay::us(10);
    wr32(reg::FALCON_ENGINE, 0x00000000); // Deassert reset
    delay::us(10);

    // Step 4: Wait for memory scrubbing to complete
    int32_t rc = wait_mem_scrub();
    if (rc != OK) return rc;

    // Step 5: Select Falcon core (GA102 has dual falcon/riscv)
    rc = select_falcon_core();
    if (rc != OK) return rc;

    // Step 6: Wait for mem scrub again (after core select)
    rc = wait_mem_scrub();
    if (rc != OK) return rc;

    // Step 7: Write RM register (PMC_BOOT_0 value)
    wr32(reg::FALCON_RM, m_gpu->boot0());

    log::info("nvidia: falcon: %s reset complete", name());
    return OK;
}

int32_t falcon::wait_mem_scrub() {
    uint64_t deadline = clock::now_ns() + 20000000; // 20ms
    while (rd32(reg::FALCON_HWCFG2) & reg::FALCON_HWCFG2_MEM_SCRUB) {
        if (clock::now_ns() >= deadline) {
            log::error("nvidia: falcon: %s memory scrubbing timeout (HWCFG2=0x%08x)",
                       name(), rd32(reg::FALCON_HWCFG2));
            return ERR_TIMEOUT;
        }
        delay::us(10);
    }
    return OK;
}

int32_t falcon::select_falcon_core() {
    uint32_t bcr = rd32(reg::FALCON_RISCV_BCR_CTRL);
    if (bcr & (1 << 4)) { // core_select != Falcon
        log::info("nvidia: falcon: %s switching from RISC-V to Falcon core", name());
        wr32(reg::FALCON_RISCV_BCR_CTRL, 0x00000000); // Select Falcon

        // Wait for valid bit
        uint64_t deadline = clock::now_ns() + 10000000; // 10ms
        while (!(rd32(reg::FALCON_RISCV_BCR_CTRL) & 0x01)) {
            if (clock::now_ns() >= deadline) {
                log::error("nvidia: falcon: %s core select timeout", name());
                return ERR_TIMEOUT;
            }
            delay::us(10);
        }
    }
    return OK;
}

// ============================================================================
// DMA Firmware Loading
// ============================================================================

int32_t falcon::configure_fbif() {
    // Allow physical addressing without context
    mask32(reg::FALCON_FBIF_CTL, 0, reg::FALCON_FBIF_CTL_ALLOW_PHYS);

    // Clear DMA control
    wr32(reg::FALCON_DMACTL, 0x00000000);

    // Configure transfer: CoherentSysmem, Physical
    uint32_t transcfg = reg::FALCON_FBIF_TARGET_COHERENT | reg::FALCON_FBIF_MEM_PHYSICAL;
    wr32(reg::FALCON_FBIF_TRANSCFG, transcfg);

    return OK;
}

int32_t falcon::dma_xfer_block(uint64_t dma_phys, uint32_t falcon_offset,
                                uint32_t src_offset, uint32_t cmd) {
    wr32(reg::FALCON_DMATRFBASE, static_cast<uint32_t>(dma_phys >> 8));
    wr32(reg::FALCON_DMATRFBASE1, static_cast<uint32_t>(dma_phys >> 40));
    wr32(reg::FALCON_DMATRFMOFFS, falcon_offset);
    wr32(reg::FALCON_DMATRFFBOFFS, src_offset);
    wr32(reg::FALCON_DMATRFCMD, cmd);

    return wait_dma_idle();
}

int32_t falcon::wait_dma_idle() {
    uint64_t deadline = clock::now_ns() + 2000000; // 2ms per block
    while (!(rd32(reg::FALCON_DMATRFCMD) & reg::FALCON_DMA_IDLE)) {
        if (clock::now_ns() >= deadline) {
            log::error("nvidia: falcon: %s DMA transfer timeout (DMATRFCMD=0x%08x)",
                       name(), rd32(reg::FALCON_DMATRFCMD));
            return ERR_TIMEOUT;
        }
        delay::us(1);
    }
    return OK;
}

int32_t falcon::dma_load(const dma::buffer& dma_buf,
                          uint32_t imem_dst, uint32_t imem_src, uint32_t imem_size,
                          uint32_t dmem_dst, uint32_t dmem_src, uint32_t dmem_size,
                          bool secure_imem) {
    log::info("nvidia: falcon: %s DMA load: IMEM %u bytes @ dst=0x%x src=0x%x, "
              "DMEM %u bytes @ dst=0x%x src=0x%x",
              name(), imem_size, imem_dst, imem_src, dmem_size, dmem_dst, dmem_src);

    int32_t rc = configure_fbif();
    if (rc != OK) return rc;

    // Build DMA command words
    uint32_t imem_cmd = reg::FALCON_DMA_SIZE_256 | reg::FALCON_DMA_IMEM;
    if (secure_imem) imem_cmd |= reg::FALCON_DMA_SEC;

    uint32_t dmem_cmd = reg::FALCON_DMA_SIZE_256; // DMEM, non-secure

    // Load IMEM in 256-byte blocks
    uint32_t imem_blocks = (imem_size + 255) / 256;
    for (uint32_t i = 0; i < imem_blocks; i++) {
        uint32_t pos = i * 256;
        rc = dma_xfer_block(dma_buf.phys, imem_dst + pos, imem_src + pos, imem_cmd);
        if (rc != OK) {
            log::error("nvidia: falcon: %s IMEM DMA failed at block %u/%u", name(), i, imem_blocks);
            return rc;
        }
    }
    log::info("nvidia: falcon: %s IMEM loaded (%u blocks)", name(), imem_blocks);

    // Load DMEM in 256-byte blocks (align size up to 256)
    uint32_t dmem_aligned = (dmem_size + 255) & ~255u;
    uint32_t dmem_blocks = dmem_aligned / 256;
    for (uint32_t i = 0; i < dmem_blocks; i++) {
        uint32_t pos = i * 256;
        rc = dma_xfer_block(dma_buf.phys, dmem_dst + pos, dmem_src + pos, dmem_cmd);
        if (rc != OK) {
            log::error("nvidia: falcon: %s DMEM DMA failed at block %u/%u", name(), i, dmem_blocks);
            return rc;
        }
    }
    log::info("nvidia: falcon: %s DMEM loaded (%u blocks)", name(), dmem_blocks);

    return OK;
}

// ============================================================================
// BROM Programming (GA102 Heavy-Secure)
// ============================================================================

int32_t falcon::program_brom(uint32_t pkc_data_offset, uint32_t engine_id_mask,
                              uint32_t ucode_id) {
    log::info("nvidia: falcon: %s BROM: pkc_off=0x%x engine_mask=0x%x ucode_id=%u",
              name(), pkc_data_offset, engine_id_mask, ucode_id);

    wr32(reg::FALCON_BROM_PARAADDR, pkc_data_offset);
    wr32(reg::FALCON_BROM_ENGIDMASK, engine_id_mask);
    wr32(reg::FALCON_BROM_UCODE_ID, ucode_id);
    wr32(reg::FALCON_BROM_MOD_SEL, 0x01); // RSA-3K

    return OK;
}

// ============================================================================
// Boot and Wait
// ============================================================================

int32_t falcon::boot_and_wait(uint32_t boot_addr, uint32_t mbox0, uint32_t mbox1,
                               uint32_t& out_mbox0, uint32_t& out_mbox1) {
    log::info("nvidia: falcon: %s boot: addr=0x%x mbox0=0x%x mbox1=0x%x",
              name(), boot_addr, mbox0, mbox1);

    // Set mailboxes
    set_mailbox0(mbox0);
    set_mailbox1(mbox1);

    // Set boot vector
    wr32(reg::FALCON_BOOTVEC, boot_addr);

    // Start CPU
    uint32_t cpuctl = rd32(reg::FALCON_CPUCTL);
    if (cpuctl & reg::FALCON_CPUCTL_ALIAS_EN) {
        wr32(reg::FALCON_CPUCTL_ALIAS, reg::FALCON_CPUCTL_STARTCPU);
    } else {
        wr32(reg::FALCON_CPUCTL, reg::FALCON_CPUCTL_STARTCPU);
    }

    // Wait for halt (2 second timeout)
    uint64_t deadline = clock::now_ns() + 2000000000ULL; // 2s
    while (!(rd32(reg::FALCON_CPUCTL) & reg::FALCON_CPUCTL_HALTED)) {
        if (clock::now_ns() >= deadline) {
            out_mbox0 = mailbox0();
            out_mbox1 = mailbox1();
            log::error("nvidia: falcon: %s boot timeout (CPUCTL=0x%08x mbox0=0x%08x mbox1=0x%08x)",
                       name(), rd32(reg::FALCON_CPUCTL), out_mbox0, out_mbox1);
            return ERR_TIMEOUT;
        }
        delay::us(100);
    }

    // Read result mailboxes
    out_mbox0 = mailbox0();
    out_mbox1 = mailbox1();

    log::info("nvidia: falcon: %s halted: mbox0=0x%08x mbox1=0x%08x",
              name(), out_mbox0, out_mbox1);

    return OK;
}

} // namespace nv
