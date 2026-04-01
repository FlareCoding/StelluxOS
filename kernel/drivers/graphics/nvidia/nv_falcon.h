#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FALCON_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FALCON_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/nv_types.h"
#include "drivers/graphics/nvidia/nv_regs.h"

namespace nv {

class nv_gpu;

/**
 * Falcon microcontroller interface.
 *
 * Handles DMA firmware loading, reset, boot, and mailbox communication
 * for the GSP and SEC2 falcons on NVIDIA Ampere GPUs.
 *
 * Reference: Linux nouveau falcon/v1.c, falcon/ga102.c, nova-core falcon.rs
 */
class falcon {
public:
    enum class engine_type : uint8_t {
        GSP  = 0,
        SEC2 = 1,
    };

    falcon() : m_gpu(nullptr), m_base(0), m_type(engine_type::GSP) {}

    /**
     * Initialize the falcon interface for a specific engine.
     */
    int32_t init(nv_gpu* gpu, engine_type type);

    /**
     * Full reset sequence (GA102):
     * 1. Disable IRQs
     * 2. Wait for reset ready
     * 3. Engine reset (assert + deassert)
     * 4. Wait for memory scrubbing
     * 5. Select Falcon core (vs RISC-V)
     * 6. Write RM register
     */
    int32_t reset();

    /**
     * Load firmware IMEM+DMEM via DMA transfer.
     *
     * @param dma_buf      DMA buffer containing the firmware data
     * @param imem_dst     Falcon IMEM destination offset
     * @param imem_src     Source offset in DMA buffer for IMEM data
     * @param imem_size    Size of IMEM data (must be 256-byte aligned)
     * @param dmem_dst     Falcon DMEM destination offset
     * @param dmem_src     Source offset in DMA buffer for DMEM data
     * @param dmem_size    Size of DMEM data (will be aligned to 256)
     * @param secure_imem  Load IMEM as secure code
     */
    int32_t dma_load(const dma::buffer& dma_buf,
                     uint32_t imem_dst, uint32_t imem_src, uint32_t imem_size,
                     uint32_t dmem_dst, uint32_t dmem_src, uint32_t dmem_size,
                     bool secure_imem);

    /**
     * Program BROM registers for GA102 Heavy-Secure boot.
     */
    int32_t program_brom(uint32_t pkc_data_offset, uint32_t engine_id_mask,
                         uint32_t ucode_id);

    /**
     * Set boot vector and mailbox values, then start the falcon.
     * Blocks until the falcon halts (or timeout).
     *
     * @param boot_addr    Entry point address
     * @param mbox0        Mailbox 0 input value
     * @param mbox1        Mailbox 1 input value
     * @param out_mbox0    Output: Mailbox 0 result
     * @param out_mbox1    Output: Mailbox 1 result
     * @return OK on success (falcon halted), ERR_TIMEOUT on timeout
     */
    int32_t boot_and_wait(uint32_t boot_addr, uint32_t mbox0, uint32_t mbox1,
                          uint32_t& out_mbox0, uint32_t& out_mbox1);

    // Direct register access (relative to falcon base)
    uint32_t rd32(uint32_t offset) const;
    void wr32(uint32_t offset, uint32_t val);
    void mask32(uint32_t offset, uint32_t clear, uint32_t set);

    // Mailbox access
    uint32_t mailbox0() const;
    uint32_t mailbox1() const;
    void set_mailbox0(uint32_t val);
    void set_mailbox1(uint32_t val);

    // Engine base address
    uint32_t base() const { return m_base; }
    const char* name() const;

private:
    int32_t configure_fbif();
    int32_t dma_xfer_block(uint64_t dma_phys, uint32_t falcon_offset,
                           uint32_t src_offset, uint32_t cmd);
    int32_t wait_dma_idle();
    int32_t select_falcon_core();
    int32_t wait_mem_scrub();

    nv_gpu*     m_gpu;
    uint32_t    m_base;
    engine_type m_type;
};

} // namespace nv

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FALCON_H
