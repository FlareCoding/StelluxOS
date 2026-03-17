#ifndef STELLUX_PCI_PCI_H
#define STELLUX_PCI_PCI_H

#include "common/types.h"

namespace pci {

constexpr int32_t OK          = 0;
constexpr int32_t ERR_NO_MCFG = -1;
constexpr int32_t ERR_MAP     = -2;

constexpr int32_t ERR_MSI_NO_CAP  = -10;
constexpr int32_t ERR_MSI_MAP     = -11;
constexpr int32_t ERR_MSI_ALREADY = -12;
constexpr int32_t ERR_MSI_ALLOC   = -13;
constexpr int32_t ERR_MSI_COMPOSE = -14;

// Config space register offsets
constexpr uint16_t CFG_VENDOR_ID      = 0x00;
constexpr uint16_t CFG_DEVICE_ID      = 0x02;
constexpr uint16_t CFG_COMMAND        = 0x04;
constexpr uint16_t CFG_STATUS         = 0x06;
constexpr uint16_t CFG_REVISION       = 0x08;
constexpr uint16_t CFG_PROG_IF        = 0x09;
constexpr uint16_t CFG_SUBCLASS       = 0x0A;
constexpr uint16_t CFG_CLASS_CODE     = 0x0B;
constexpr uint16_t CFG_HEADER_TYPE    = 0x0E;
constexpr uint16_t CFG_BAR0           = 0x10;
constexpr uint16_t CFG_CAP_PTR        = 0x34;
constexpr uint16_t CFG_INTERRUPT_LINE = 0x3C;
constexpr uint16_t CFG_INTERRUPT_PIN  = 0x3D;

// Command register bits
constexpr uint16_t CMD_IO_SPACE          = (1 << 0);
constexpr uint16_t CMD_MEMORY_SPACE      = (1 << 1);
constexpr uint16_t CMD_BUS_MASTER        = (1 << 2);
constexpr uint16_t CMD_INTERRUPT_DISABLE = (1 << 10);

// Status register bits
constexpr uint16_t STS_CAPABILITIES = (1 << 4);

// Header type bits
constexpr uint8_t HDR_TYPE_MASK      = 0x7F;
constexpr uint8_t HDR_MULTI_FUNCTION = 0x80;
constexpr uint8_t HDR_TYPE_NORMAL    = 0x00;
constexpr uint8_t HDR_TYPE_BRIDGE    = 0x01;

// Capability IDs
constexpr uint8_t CAP_MSI  = 0x05;
constexpr uint8_t CAP_PCIE = 0x10;
constexpr uint8_t CAP_MSIX = 0x11;

// BAR type constants
constexpr uint8_t BAR_NONE   = 0;
constexpr uint8_t BAR_IO     = 1;
constexpr uint8_t BAR_MMIO32 = 2;
constexpr uint8_t BAR_MMIO64 = 3;

// BAR detection masks
constexpr uint32_t BAR_IO_BIT        = 0x01;
constexpr uint32_t BAR_TYPE_MASK     = 0x06;
constexpr uint32_t BAR_TYPE_64       = 0x04;
constexpr uint32_t BAR_PREFETCH_BIT  = 0x08;
constexpr uint32_t BAR_ADDR_MASK_MEM = 0xFFFFFFF0;
constexpr uint32_t BAR_ADDR_MASK_IO  = 0xFFFFFFFC;

constexpr uint8_t MAX_BARS = 6;
constexpr uint8_t MAX_CAPS = 16;
constexpr uint32_t MAX_DEVICES = 64;

struct bar {
    uint64_t phys;
    uint64_t size;
    uint8_t type;
    bool prefetchable;
};

struct capability {
    uint8_t id;
    uint8_t offset;
};

constexpr uint8_t MSI_MODE_NONE = 0;
constexpr uint8_t MSI_MODE_MSI  = 1;
constexpr uint8_t MSI_MODE_MSIX = 2;

struct msi_state {
    uint32_t  base_vector;
    uint16_t  vector_count;
    uint8_t   mode;
    uint8_t   cap_offset;
    uintptr_t msix_table_va;
};
static_assert(sizeof(msi_state) <= 24, "msi_state should stay compact");

class device {
public:
    uint8_t bus() const { return m_bus; }
    uint8_t slot() const { return m_slot; }
    uint8_t func() const { return m_func; }
    uint16_t vendor_id() const { return m_vendor_id; }
    uint16_t device_id() const { return m_device_id; }
    uint8_t class_code() const { return m_class_code; }
    uint8_t subclass() const { return m_subclass; }
    uint8_t prog_if() const { return m_prog_if; }
    uint8_t header_type() const { return m_header_type; }
    uint8_t interrupt_pin() const { return m_interrupt_pin; }

    const bar& get_bar(uint8_t index) const;
    bool has_capability(uint8_t cap_id) const;
    uint8_t capability_offset(uint8_t cap_id) const;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE uint8_t config_read8(uint16_t offset) const;
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE uint16_t config_read16(uint16_t offset) const;
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE uint32_t config_read32(uint16_t offset) const;
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void config_write8(uint16_t offset, uint8_t val);
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void config_write16(uint16_t offset, uint16_t val);
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void config_write32(uint16_t offset, uint32_t val);

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void enable();
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void enable_bus_mastering();
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void disable();

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE int32_t enable_msi(uint32_t requested_count,
                                         uint32_t target_cpu);
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE int32_t enable_msix(uint32_t requested_count,
                                          uint32_t target_cpu);
    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void disable_msi();

    const msi_state& get_msi_state() const { return m_msi; }

private:
    /** @note Privilege: **required** */
    friend __PRIVILEGED_CODE int32_t init();
    /** @note Privilege: **required** */
    friend __PRIVILEGED_CODE void enumerate_function(uint8_t, uint8_t, uint8_t);
    /** @note Privilege: **required** */
    friend __PRIVILEGED_CODE void parse_bars(device&);
    /** @note Privilege: **required** */
    friend __PRIVILEGED_CODE void parse_capabilities(device&);

    uint8_t   m_bus;
    uint8_t   m_slot;
    uint8_t   m_func;
    uint16_t  m_vendor_id;
    uint16_t  m_device_id;
    uint8_t   m_class_code;
    uint8_t   m_subclass;
    uint8_t   m_prog_if;
    uint8_t   m_header_type;
    uint8_t   m_interrupt_pin;
    uintptr_t m_ecam_base;
    bar        m_bars[MAX_BARS];
    capability m_caps[MAX_CAPS];
    uint8_t   m_cap_count;
    msi_state m_msi;
};

/**
 * Parse MCFG, map ECAM, enumerate bus 0, parse BARs and capabilities.
 * Idempotent. Call after acpi::init() and mm::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

uint32_t device_count();
device* get_device(uint32_t index);
[[nodiscard]] device* find_by_class(uint8_t class_code, uint8_t subclass);
[[nodiscard]] device* find_by_progif(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

} // namespace pci

#endif // STELLUX_PCI_PCI_H
