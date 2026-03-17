#include "arch/arch_msi.h"
#include "acpi/madt_arch.h"
#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "mm/vmm.h"
#include "hw/mmio.h"
#include "common/logging.h"

namespace arch {

static constexpr uint32_t V2M_MSI_TYPER     = 0x008;
static constexpr uint32_t V2M_MSI_SETSPI_NS = 0x040;
static constexpr uint32_t V2M_FRAME_SIZE    = 0x1000;

__PRIVILEGED_BSS static uint64_t  g_frame_phys;
__PRIVILEGED_BSS static uintptr_t g_frame_va;
__PRIVILEGED_BSS static uint32_t  g_spi_base;
__PRIVILEGED_BSS static uint32_t  g_spi_count;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_init(uint32_t* out_capacity) {
    const auto& madt = acpi::get_madt_info();

    if (madt.msi_frame.base_address == 0) {
        *out_capacity = 0;
        return msi::ERR_NOT_SUPPORTED;
    }

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
__PRIVILEGED_CODE int32_t msi_compose(uint32_t vector, uint32_t target_cpu,
                                      msi::message* out) {
    if (vector >= g_spi_count || out == nullptr) {
        return msi::ERR_INVALID;
    }

    if (target_cpu >= 8) {
        return msi::ERR_INVALID;
    }

    out->address = g_frame_phys + V2M_MSI_SETSPI_NS;
    out->data = g_spi_base + vector;

    irq::set_spi_target(g_spi_base + vector,
                        static_cast<uint8_t>(1u << target_cpu));

    return msi::OK;
}

/**
 * Handle an IRQ if it is a GICv2m MSI. Dispatches the handler and
 * sends EOI. Returns true if the IRQ was handled.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool msi_handle_irq(uint32_t irq_id) {
    if (g_spi_count == 0) {
        return false;
    }
    if (irq_id >= g_spi_base && irq_id < g_spi_base + g_spi_count) {
        msi::dispatch(irq_id - g_spi_base);
        irq::eoi(irq_id);
        return true;
    }
    return false;
}

} // namespace arch
