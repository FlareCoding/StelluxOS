#include "arch/arch_msi.h"
#include "acpi/madt_arch.h"
#include "pci/pci.h"
#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "mm/vmm.h"
#include "hw/mmio.h"
#include "common/logging.h"

namespace arch {

// Backend selection
enum class msi_backend : uint8_t { NONE, GICV2M, BCM2711 };

__PRIVILEGED_BSS static msi_backend g_backend;

// GICv2m constants
static constexpr uint32_t V2M_MSI_TYPER     = 0x008;
static constexpr uint32_t V2M_MSI_SETSPI_NS = 0x040;
static constexpr uint32_t V2M_FRAME_SIZE    = 0x1000;

// GICv2m state
__PRIVILEGED_BSS static uint64_t  g_frame_phys;
__PRIVILEGED_BSS static uintptr_t g_frame_va;
__PRIVILEGED_BSS static uint32_t  g_spi_base;

// BCM2711 constants
static constexpr uint32_t BCM_MSI_BAR_CONFIG_LO  = 0x4044;
static constexpr uint32_t BCM_MSI_BAR_CONFIG_HI  = 0x4048;
static constexpr uint32_t BCM_MSI_DATA_CONFIG     = 0x404C;
static constexpr uint32_t BCM_MSI_DATA_CONFIG_VAL = 0xFFE06540;
static constexpr uint64_t BCM_MSI_TARGET_ADDR     = 0x0FFFFFFFCULL;
static constexpr uint32_t BCM_MSI_INTR2_STATUS    = 0x4500;
static constexpr uint32_t BCM_MSI_INTR2_CLR       = 0x4508;
static constexpr uint32_t BCM_MSI_INTR2_MASK_CLR  = 0x4514;
static constexpr uint32_t BCM_MSI_SPI_INTID       = 180;
static constexpr uint32_t BCM_MSI_VECTOR_COUNT    = 32;

// BCM2711 state
__PRIVILEGED_BSS static uintptr_t g_brcm_base;

// Shared state (used by both backends)
__PRIVILEGED_BSS static uint32_t g_spi_count;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t msi_init_gicv2m(const acpi::madt_info& madt,
                                                  uint32_t* out_capacity) {
    g_frame_phys = madt.msi_frame.base_address;

    uintptr_t map_base = 0;
    uintptr_t map_va = 0;
    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(g_frame_phys),
        V2M_FRAME_SIZE,
        paging::PAGE_READ | paging::PAGE_WRITE,
        map_base, map_va);
    if (rc != vmm::OK) {
        log::error("msi: failed to map GICv2m frame at 0x%lx (%d)",
                   g_frame_phys, rc);
        *out_capacity = 0;
        return msi::ERR_INIT;
    }
    (void)map_base;
    g_frame_va = map_va;

    if (madt.msi_frame.flags & acpi::GIC_MSI_FRAME_FLAG_SPI_SELECT) {
        g_spi_base = madt.msi_frame.spi_base;
        g_spi_count = madt.msi_frame.spi_count;
    } else {
        uint32_t typer = mmio::read32(g_frame_va + V2M_MSI_TYPER);
        g_spi_base = (typer >> 16) & 0x3FF;
        g_spi_count = typer & 0x3FF;
    }

    if (g_spi_base < 32 || g_spi_count == 0 || g_spi_base + g_spi_count > 1020) {
        log::error("msi: invalid GICv2m SPI range (base=%u count=%u)",
                   g_spi_base, g_spi_count);
        *out_capacity = 0;
        return msi::ERR_INIT;
    }

    if (g_spi_count > msi::MAX_VECTORS) {
        g_spi_count = msi::MAX_VECTORS;
    }

    for (uint32_t i = 0; i < g_spi_count; i++) {
        uint32_t intid = g_spi_base + i;
        irq::set_edge_triggered(intid);
        irq::set_spi_target(intid, 0x01);
        irq::unmask(intid);
    }

    *out_capacity = g_spi_count;

    log::info("msi: GICv2m backend, %u vectors (SPI %u-%u, doorbell=0x%lx)",
              g_spi_count, g_spi_base, g_spi_base + g_spi_count - 1,
              g_frame_phys + V2M_MSI_SETSPI_NS);
    return msi::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t msi_init_bcm2711(uint32_t* out_capacity) {
    g_brcm_base = pci::brcm_controller_base();

    // Unmask all 32 MSI vectors
    mmio::write32(g_brcm_base + BCM_MSI_INTR2_MASK_CLR, 0xFFFFFFFF);

    // Clear all pending
    mmio::write32(g_brcm_base + BCM_MSI_INTR2_CLR, 0xFFFFFFFF);

    // Program MSI target address (bit 0 = enable)
    mmio::write32(g_brcm_base + BCM_MSI_BAR_CONFIG_LO,
                  static_cast<uint32_t>(BCM_MSI_TARGET_ADDR) | 0x1);
    mmio::write32(g_brcm_base + BCM_MSI_BAR_CONFIG_HI, 0);

    // Program data match config (32-vector mode)
    mmio::write32(g_brcm_base + BCM_MSI_DATA_CONFIG, BCM_MSI_DATA_CONFIG_VAL);

    // Configure the chained GIC SPI (level-triggered, group1, target CPU 0)
    irq::set_spi_target(BCM_MSI_SPI_INTID, 0x01);
    irq::set_group1(BCM_MSI_SPI_INTID);
    irq::set_level_triggered(BCM_MSI_SPI_INTID);
    irq::unmask(BCM_MSI_SPI_INTID);

    g_spi_count = BCM_MSI_VECTOR_COUNT;
    if (g_spi_count > msi::MAX_VECTORS) {
        g_spi_count = msi::MAX_VECTORS;
    }
    *out_capacity = g_spi_count;

    log::info("msi: BCM2711 backend, %u vectors (chained SPI INTID %u)",
              g_spi_count, BCM_MSI_SPI_INTID);
    return msi::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_init(uint32_t* out_capacity) {
    const auto& madt = acpi::get_madt_info();

    // Try GICv2m first (QEMU virt, generic ARM servers)
    if (madt.msi_frame.base_address != 0) {
        int32_t rc = msi_init_gicv2m(madt, out_capacity);
        if (rc == msi::OK) {
            g_backend = msi_backend::GICV2M;
            return msi::OK;
        }
        return rc;
    }

    // Fallback: BCM2711 proprietary MSI controller (Raspberry Pi 4)
    if (pci::brcm_controller_base() != 0) {
        int32_t rc = msi_init_bcm2711(out_capacity);
        if (rc == msi::OK) {
            g_backend = msi_backend::BCM2711;
            return msi::OK;
        }
        return rc;
    }

    *out_capacity = 0;
    return msi::ERR_NOT_SUPPORTED;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_compose(uint32_t vector, uint32_t target_cpu,
                                      msi::message* out) {
    if (vector >= g_spi_count || out == nullptr) {
        return msi::ERR_INVALID;
    }

    if (g_backend == msi_backend::GICV2M) {
        if (target_cpu >= 8) {
            return msi::ERR_INVALID;
        }
        out->address = g_frame_phys + V2M_MSI_SETSPI_NS;
        out->data = g_spi_base + vector;
        irq::set_spi_target(g_spi_base + vector,
                            static_cast<uint8_t>(1u << target_cpu));
        return msi::OK;
    }

    if (g_backend == msi_backend::BCM2711) {
        out->address = BCM_MSI_TARGET_ADDR;
        out->data = (0xFFFF & BCM_MSI_DATA_CONFIG_VAL) | vector;
        return msi::OK;
    }

    return msi::ERR_INVALID;
}

/**
 * Handle an IRQ if it is an MSI. Dispatches the handler and sends EOI.
 * GICv2m: range-checks irq_id against MSI SPI range.
 * BCM2711: checks for chained SPI, reads status, clears and dispatches per bit.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool msi_handle_irq(uint32_t irq_id) {
    if (g_backend == msi_backend::GICV2M) {
        if (irq_id >= g_spi_base && irq_id < g_spi_base + g_spi_count) {
            msi::dispatch(irq_id - g_spi_base);
            irq::eoi(irq_id);
            return true;
        }
        return false;
    }

    if (g_backend == msi_backend::BCM2711) {
        if (irq_id != BCM_MSI_SPI_INTID) {
            return false;
        }
        uint32_t status = mmio::read32(g_brcm_base + BCM_MSI_INTR2_STATUS);
        while (status != 0) {
            uint32_t bit = __builtin_ctz(status);
            mmio::write32(g_brcm_base + BCM_MSI_INTR2_CLR, 1u << bit);
            msi::dispatch(bit);
            status &= ~(1u << bit);
        }
        irq::eoi(irq_id);
        return true;
    }

    return false;
}

} // namespace arch
