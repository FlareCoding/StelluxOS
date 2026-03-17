#include "pci/pci.h"
#include "msi/msi.h"
#include "arch/arch_msi.h"
#include "mm/vmm.h"
#include "hw/mmio.h"
#include "common/logging.h"

namespace pci {

// MSI capability (cap ID 0x05) register offsets from cap base
static constexpr uint8_t MSI_REG_CTRL    = 0x02;
static constexpr uint8_t MSI_REG_ADDR_LO = 0x04;
static constexpr uint8_t MSI_REG_ADDR_HI = 0x08;
static constexpr uint8_t MSI_REG_DATA_32 = 0x08;
static constexpr uint8_t MSI_REG_DATA_64 = 0x0C;

// MSI Message Control bits
static constexpr uint16_t MSI_CTRL_ENABLE    = (1 << 0);
static constexpr uint16_t MSI_CTRL_MMC_SHIFT = 1;
static constexpr uint16_t MSI_CTRL_MMC_MASK  = (0x7 << 1);
static constexpr uint16_t MSI_CTRL_MME_SHIFT = 4;
static constexpr uint16_t MSI_CTRL_MME_MASK  = (0x7 << 4);
static constexpr uint16_t MSI_CTRL_64BIT     = (1 << 7);

// MSI-X capability (cap ID 0x11) register offsets from cap base
static constexpr uint8_t MSIX_REG_CTRL  = 0x02;
static constexpr uint8_t MSIX_REG_TABLE = 0x04;

// MSI-X Message Control bits
static constexpr uint16_t MSIX_CTRL_ENABLE    = (1 << 15);
static constexpr uint16_t MSIX_CTRL_FUNC_MASK = (1 << 14);
static constexpr uint16_t MSIX_CTRL_SIZE_MASK = 0x7FF;

// MSI-X BIR / offset extraction
static constexpr uint32_t MSIX_BIR_MASK    = 0x7;
static constexpr uint32_t MSIX_OFFSET_MASK = ~0x7u;

// MSI-X table entry layout (16 bytes per entry in BAR MMIO)
static constexpr uint32_t MSIX_ENTRY_SIZE       = 16;
static constexpr uint32_t MSIX_ENTRY_ADDR_LO    = 0x00;
static constexpr uint32_t MSIX_ENTRY_ADDR_HI    = 0x04;
static constexpr uint32_t MSIX_ENTRY_DATA        = 0x08;
static constexpr uint32_t MSIX_ENTRY_VECTOR_CTRL = 0x0C;
static constexpr uint32_t MSIX_ENTRY_MASKED      = (1 << 0);

static constexpr uint32_t round_down_pow2(uint32_t n) {
    if (n == 0) {
        return 0;
    }
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return (n >> 1) + 1;
}

static constexpr uint32_t mmc_to_count(uint16_t mmc_field) {
    return 1u << mmc_field;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t device::enable_msi(uint32_t requested_count,
                                              uint32_t target_cpu) {
    if (m_msi.mode != MSI_MODE_NONE) {
        return ERR_MSI_ALREADY;
    }

    uint8_t cap_off = capability_offset(CAP_MSI);
    if (cap_off == 0) {
        return ERR_MSI_NO_CAP;
    }

    uint16_t ctrl = config_read16(cap_off + MSI_REG_CTRL);
    bool is_64bit = (ctrl & MSI_CTRL_64BIT) != 0;

    uint16_t mmc_field = (ctrl & MSI_CTRL_MMC_MASK) >> MSI_CTRL_MMC_SHIFT;
    uint32_t max_vectors = mmc_to_count(mmc_field);

    uint32_t count = requested_count;
    if (count > max_vectors) {
        count = max_vectors;
    }
    count = round_down_pow2(count);
    if (count == 0) {
        count = 1;
    }

    uint32_t base = 0;
    int32_t rc = msi::alloc(count, count, &base);
    if (rc != msi::OK) {
        return ERR_MSI_ALLOC;
    }

    msi::message msg = {};
    rc = arch::msi_compose(base, target_cpu, &msg);
    if (rc != msi::OK) {
        msi::free(base, count);
        return ERR_MSI_COMPOSE;
    }

    // Disable MSI before reprogramming (preserve reserved bits)
    ctrl = config_read16(cap_off + MSI_REG_CTRL);
    ctrl &= ~MSI_CTRL_ENABLE;
    config_write16(cap_off + MSI_REG_CTRL, ctrl);

    config_write32(cap_off + MSI_REG_ADDR_LO,
                   static_cast<uint32_t>(msg.address & 0xFFFFFFFF));

    if (is_64bit) {
        config_write32(cap_off + MSI_REG_ADDR_HI,
                       static_cast<uint32_t>(msg.address >> 32));
    }

    uint8_t data_offset = is_64bit ? MSI_REG_DATA_64 : MSI_REG_DATA_32;
    config_write16(cap_off + data_offset,
                   static_cast<uint16_t>(msg.data & 0xFFFF));

    // Set MME and enable in a single write
    uint16_t mme_val = static_cast<uint16_t>(__builtin_ctz(count));
    ctrl = config_read16(cap_off + MSI_REG_CTRL);
    ctrl &= ~MSI_CTRL_MME_MASK;
    ctrl |= (mme_val << MSI_CTRL_MME_SHIFT) & MSI_CTRL_MME_MASK;
    ctrl |= MSI_CTRL_ENABLE;
    config_write16(cap_off + MSI_REG_CTRL, ctrl);

    // Disable legacy INTx
    uint16_t cmd = config_read16(CFG_COMMAND);
    cmd |= CMD_INTERRUPT_DISABLE;
    config_write16(CFG_COMMAND, cmd);

    m_msi.base_vector = base;
    m_msi.vector_count = static_cast<uint16_t>(count);
    m_msi.mode = MSI_MODE_MSI;
    m_msi.cap_offset = cap_off;
    m_msi.msix_table_va = 0;

    log::info("pci: MSI enabled on %02x:%02x.%x (%u vector%s, base=%u)",
              m_bus, m_slot, m_func, count, count > 1 ? "s" : "", base);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t device::enable_msix(uint32_t requested_count,
                                               uint32_t target_cpu) {
    if (m_msi.mode != MSI_MODE_NONE) {
        return ERR_MSI_ALREADY;
    }

    uint8_t cap_off = capability_offset(CAP_MSIX);
    if (cap_off == 0) {
        return ERR_MSI_NO_CAP;
    }

    uint16_t ctrl = config_read16(cap_off + MSIX_REG_CTRL);
    uint32_t table_size = (ctrl & MSIX_CTRL_SIZE_MASK) + 1;

    uint32_t count = requested_count;
    if (count > table_size) {
        count = table_size;
    }

    uint32_t table_reg = config_read32(cap_off + MSIX_REG_TABLE);
    uint8_t bir = static_cast<uint8_t>(table_reg & MSIX_BIR_MASK);
    uint32_t table_offset = table_reg & MSIX_OFFSET_MASK;

    const bar& table_bar = get_bar(bir);
    if (table_bar.type == BAR_NONE) {
        return ERR_MSI_MAP;
    }

    uintptr_t map_base = 0;
    uintptr_t table_va = 0;
    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(table_bar.phys + table_offset),
        static_cast<size_t>(count) * MSIX_ENTRY_SIZE,
        paging::PAGE_READ | paging::PAGE_WRITE,
        map_base, table_va);
    if (rc != vmm::OK) {
        log::error("pci: MSI-X table map failed on %02x:%02x.%x (%d)",
                   m_bus, m_slot, m_func, rc);
        return ERR_MSI_MAP;
    }

    uint32_t base = 0;
    rc = msi::alloc(count, 1, &base);
    if (rc != msi::OK) {
        vmm::free(table_va);
        return ERR_MSI_ALLOC;
    }

    // Set Enable=1 + FuncMask=1 to allow table writes while blocking delivery
    ctrl = config_read16(cap_off + MSIX_REG_CTRL);
    ctrl |= MSIX_CTRL_ENABLE | MSIX_CTRL_FUNC_MASK;
    config_write16(cap_off + MSIX_REG_CTRL, ctrl);

    for (uint32_t i = 0; i < count; i++) {
        msi::message msg = {};
        rc = arch::msi_compose(base + i, target_cpu, &msg);
        if (rc != msi::OK) {
            msi::free(base, count);
            // Disable MSI-X before unmapping
            ctrl = config_read16(cap_off + MSIX_REG_CTRL);
            ctrl &= ~MSIX_CTRL_ENABLE;
            config_write16(cap_off + MSIX_REG_CTRL, ctrl);
            vmm::free(table_va);
            return ERR_MSI_COMPOSE;
        }

        uintptr_t entry_addr = table_va + static_cast<uintptr_t>(i) * MSIX_ENTRY_SIZE;
        mmio::write32(entry_addr + MSIX_ENTRY_ADDR_LO,
                      static_cast<uint32_t>(msg.address & 0xFFFFFFFF));
        mmio::write32(entry_addr + MSIX_ENTRY_ADDR_HI,
                      static_cast<uint32_t>(msg.address >> 32));
        mmio::write32(entry_addr + MSIX_ENTRY_DATA, msg.data);
        mmio::write32(entry_addr + MSIX_ENTRY_VECTOR_CTRL, 0); // unmask
    }

    // Mask unused entries
    for (uint32_t i = count; i < table_size; i++) {
        uintptr_t entry_addr = table_va + static_cast<uintptr_t>(i) * MSIX_ENTRY_SIZE;
        mmio::write32(entry_addr + MSIX_ENTRY_VECTOR_CTRL, MSIX_ENTRY_MASKED);
    }

    // Clear FuncMask -- delivery starts
    ctrl = config_read16(cap_off + MSIX_REG_CTRL);
    ctrl &= ~MSIX_CTRL_FUNC_MASK;
    config_write16(cap_off + MSIX_REG_CTRL, ctrl);

    // Disable legacy INTx
    uint16_t cmd = config_read16(CFG_COMMAND);
    cmd |= CMD_INTERRUPT_DISABLE;
    config_write16(CFG_COMMAND, cmd);

    m_msi.base_vector = base;
    m_msi.vector_count = static_cast<uint16_t>(count);
    m_msi.mode = MSI_MODE_MSIX;
    m_msi.cap_offset = cap_off;
    m_msi.msix_table_va = table_va;

    log::info("pci: MSI-X enabled on %02x:%02x.%x (%u/%u entries, base=%u)",
              m_bus, m_slot, m_func, count, table_size, base);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void device::disable_msi() {
    if (m_msi.mode == MSI_MODE_NONE) {
        return;
    }

    if (m_msi.mode == MSI_MODE_MSI) {
        uint16_t ctrl = config_read16(m_msi.cap_offset + MSI_REG_CTRL);
        ctrl &= ~MSI_CTRL_ENABLE;
        config_write16(m_msi.cap_offset + MSI_REG_CTRL, ctrl);
    } else if (m_msi.mode == MSI_MODE_MSIX) {
        // Set FuncMask to block all delivery first
        uint16_t ctrl = config_read16(m_msi.cap_offset + MSIX_REG_CTRL);
        ctrl |= MSIX_CTRL_FUNC_MASK;
        config_write16(m_msi.cap_offset + MSIX_REG_CTRL, ctrl);

        // Clear enable
        ctrl &= ~MSIX_CTRL_ENABLE;
        config_write16(m_msi.cap_offset + MSIX_REG_CTRL, ctrl);

        // Release the table mapping
        if (m_msi.msix_table_va != 0) {
            vmm::free(m_msi.msix_table_va);
        }
    }

    msi::free(m_msi.base_vector, m_msi.vector_count);
    m_msi = msi_state{};
}

} // namespace pci
