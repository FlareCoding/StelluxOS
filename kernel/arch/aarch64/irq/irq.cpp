#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "irq/gic.h"
#include "acpi/madt_arch.h"
#include "common/logging.h"

namespace irq {

// The MADT version field arrived with GICv3, so firmware that leaves it at zero
// describes a GICv2. GICv4 keeps the GICv3 programming model used here.
constexpr uint8_t GIC_VERSION_3 = 3;

__PRIVILEGED_BSS static const gic_backend* g_gic;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    const auto& madt = acpi::get_madt_info();

    if (madt.gicd_base == 0 || madt.cpu_count == 0) {
        log::error("irq: no GIC info in MADT");
        return ERR_NO_MADT;
    }

    g_gic = madt.gic_version >= GIC_VERSION_3 ? &gicv3_backend() : &gicv2_backend();
    return g_gic->init(madt);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap() {
    return g_gic->init_ap();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t acknowledge() {
    return g_gic->acknowledge();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void eoi(uint32_t irq) {
    g_gic->eoi(irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t send_sgi(uint32_t intid, uint32_t target_cpu) {
    return g_gic->send_sgi(intid, target_cpu);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_spi_target(uint32_t irq, uint32_t target_cpu) {
    return g_gic->set_spi_target(irq, target_cpu);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmask(uint32_t irq) {
    g_gic->unmask(irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mask(uint32_t irq) {
    g_gic->mask(irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_group1(uint32_t irq) {
    g_gic->set_group1(irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_level_triggered(uint32_t irq) {
    g_gic->set_trigger(irq, trigger::level);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_edge_triggered(uint32_t irq) {
    g_gic->set_trigger(irq, trigger::edge);
}

} // namespace irq
