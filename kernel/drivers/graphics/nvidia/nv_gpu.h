#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GPU_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GPU_H

#include "drivers/pci_driver.h"
#include "drivers/graphics/nvidia/nv_types.h"
#include "drivers/graphics/nvidia/nv_regs.h"

namespace nv {

/**
 * NVIDIA GPU PCI driver for display output.
 *
 * Targets RTX 3080 (GA102 / Ampere) but designed to identify any NVIDIA GPU.
 * Supports display modesetting without 3D acceleration.
 *
 * Initialization sequence:
 *   1. PCI discovery + BAR0 mapping
 *   2. Chip identification via PMC_BOOT_0
 *   3. Wait for GPU firmware (GFW) boot completion
 *   4. VBIOS reading and DCB/I2C table parsing
 *   5. EDID reading from connected monitors
 *   6. Display engine initialization and scanout
 */
class nv_gpu : public drivers::pci_driver {
public:
    nv_gpu(pci::device* dev);

    int32_t attach() override;
    void run() override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

    // BAR0 MMIO register access
    uint32_t reg_rd32(uint32_t offset) const;
    void reg_wr32(uint32_t offset, uint32_t val);
    void reg_mask32(uint32_t offset, uint32_t mask, uint32_t val);

    // Chip info accessors
    uint32_t chipset() const { return m_chipset; }
    uint8_t  chiprev() const { return m_chiprev; }
    gpu_family family() const { return m_family; }
    uint32_t boot0() const { return m_boot0; }
    const char* family_name() const;
    const char* chipset_name() const;

    // BAR accessors
    uintptr_t bar0_va() const { return m_bar0_va; }
    uintptr_t bar1_va() const { return m_bar1_va; }
    uint64_t  bar1_size() const { return m_bar1_size; }
    uint64_t  bar0_phys() const { return m_bar0_phys; }
    uint64_t  bar1_phys() const { return m_bar1_phys; }
    uint64_t  bar3_phys() const { return m_bar3_phys; }
    uint32_t  pci_bdf() const { return m_pci_bdf; }

    // VBIOS data accessors
    const uint8_t* vbios_data() const { return m_vbios; }
    uint32_t vbios_size() const { return m_vbios_size; }
    uint32_t bit_offset() const { return m_bit_offset; }

    // Parsed tables
    dcb_table& get_dcb() { return m_dcb; }
    const dcb_table& get_dcb() const { return m_dcb; }
    i2c_table& get_i2c_table() { return m_i2c; }
    const i2c_table& get_i2c_table() const { return m_i2c; }
    connector_table& get_connector_table() { return m_connectors; }
    const connector_table& get_connector_table() const { return m_connectors; }

    // Monitor EDID results
    const edid_info& get_edid(uint32_t index) const;
    uint32_t edid_count() const { return m_edid_count; }

    // Display state
    uint8_t head_mask() const { return m_head_mask; }
    uint8_t sor_mask() const { return m_sor_mask; }

private:
    // Phase 1: GPU discovery and identification
    int32_t map_bars();
    int32_t identify_chip();
    int32_t wait_gfw_boot();
    bool    is_display_disabled();

    // Phase 2: VBIOS reading and parsing
    int32_t read_vbios();
    int32_t read_vbios_prom();
    int32_t read_vbios_pramin();
    int32_t read_vbios_pci_rom();
    int32_t parse_vbios();
    int32_t find_bit_table();
    int32_t parse_dcb();
    int32_t parse_i2c_table();
    int32_t parse_connector_table();

    // Phase 3: I2C and EDID
    int32_t probe_monitors();
    int32_t i2c_init_port(uint8_t port);
    int32_t i2c_read_edid(uint8_t port, uint8_t* edid_buf);
    int32_t parse_edid(const uint8_t* raw, edid_info* out);
    int32_t aux_read_edid(uint8_t aux_ch, uint8_t* edid_buf);

    // I2C bit-bang primitives
    void i2c_scl_set(uint8_t port, bool high);
    void i2c_sda_set(uint8_t port, bool high);
    bool i2c_scl_get(uint8_t port);
    bool i2c_sda_get(uint8_t port);
    bool i2c_raise_scl(uint8_t port);
    void i2c_start(uint8_t port);
    void i2c_stop(uint8_t port);
    bool i2c_write_byte(uint8_t port, uint8_t byte);
    uint8_t i2c_read_byte(uint8_t port, bool ack);

    // Pad mode switching (hybrid I2C/AUX pads)
    void pad_set_i2c_mode(uint8_t pad);
    void pad_set_aux_mode(uint8_t pad);

    // DP AUX channel
    int32_t aux_init(uint8_t ch);
    int32_t aux_xfer(uint8_t ch, uint32_t type, uint32_t addr,
                     uint8_t* buf, uint32_t len, uint32_t* reply);
    int32_t aux_native_read(uint8_t ch, uint32_t addr,
                            uint8_t* buf, uint32_t len);
    int32_t aux_i2c_read(uint8_t ch, uint8_t dev_addr,
                         uint8_t reg, uint8_t* buf, uint32_t len);

    // Phase 4: Display engine initialization
    int32_t init_display();
    int32_t discover_display_caps();
    int32_t claim_display_ownership();
    int32_t enable_display_engine();
    int32_t setup_display_interrupts();
    int32_t program_head(uint8_t head, const edid_info& mode, uint8_t sor_id);
    int32_t program_sor(uint8_t sor_id, uint8_t head,
                        dcb_output_type type, bool hdmi, uint8_t or_mask);
    int32_t program_vpll(uint8_t head, uint32_t pixel_clock_khz);
    int32_t power_up_sor(uint8_t sor_id);
    int32_t setup_scanout(uint8_t head, uint32_t width, uint32_t height,
                          uint32_t pitch, uint32_t vram_offset);
    int32_t fill_framebuffer(uint32_t vram_offset, uint32_t width,
                             uint32_t height, uint32_t pitch, uint32_t color);

    // Logging helpers
    void log_chip_info();
    void log_bar_info();
    void log_dcb_entry(uint32_t idx, const dcb_entry& e);
    void log_i2c_port(uint32_t idx, const i2c_port& p);
    void log_connector(uint32_t idx, const connector_entry& c);
    void log_edid(uint32_t idx, const edid_info& e);

    static const char* output_type_name(dcb_output_type type);
    static const char* connector_type_name(dcb_connector_type type);

    // BAR mappings (VA for MMIO access, phys for GSP-RM RPC payloads)
    uintptr_t m_bar0_va;
    uintptr_t m_bar1_va;
    uint64_t  m_bar1_size;
    uint64_t  m_bar0_phys;
    uint64_t  m_bar1_phys;
    uint64_t  m_bar3_phys;
    uint32_t  m_pci_bdf;

    // Chip identification
    uint32_t   m_boot0;
    uint32_t   m_chipset;
    uint8_t    m_chiprev;
    gpu_family m_family;

    // VBIOS
    uint8_t  m_vbios[VBIOS_MAX_SIZE];
    uint32_t m_vbios_size;
    uint32_t m_bit_offset; // BIT table offset within VBIOS

    // Parsed tables
    dcb_table       m_dcb;
    i2c_table       m_i2c;
    connector_table m_connectors;

    // EDID results (indexed by m_edid_count, not DCB index)
    edid_info m_edid[MAX_DCB_ENTRIES];
    uint8_t   m_edid_dcb_index[MAX_DCB_ENTRIES]; // DCB entry index for each EDID
    uint32_t  m_edid_count;

    // Display capabilities
    uint8_t m_head_mask;  // Bitmask of available heads
    uint8_t m_sor_mask;   // Bitmask of available SORs
    uint8_t m_head_count;
    uint8_t m_sor_count;

    // Active display output state (for multi-monitor)
    uint8_t m_active_heads;  // Bitmask of heads currently driving outputs
    uint8_t m_active_sors;   // Bitmask of SORs currently in use
};

} // namespace nv

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GPU_H
