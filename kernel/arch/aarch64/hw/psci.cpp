#include "hw/psci.h"
#include "acpi/acpi.h"

namespace psci {

constexpr uint64_t ID_AA64PFR0_EL3_SHIFT = 12;

static inline uint64_t read_id_aa64pfr0_el1() {
    uint64_t val;
    asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(val));
    return val;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE conduit detect_conduit() {
    const acpi::fadt* fadt = acpi::get_fadt();
    if (fadt && FADT_HAS_FIELD(fadt, arm_boot_arch)) {
        if (fadt->arm_boot_arch & acpi::FADT_ARM_PSCI_COMPLIANT) {
            return (fadt->arm_boot_arch & acpi::FADT_ARM_PSCI_USE_HVC)
                ? conduit::HVC
                : conduit::SMC;
        }
    }

    bool has_el3 = ((read_id_aa64pfr0_el1() >> ID_AA64PFR0_EL3_SHIFT) & 0xF) != 0;
    return has_el3 ? conduit::SMC : conduit::HVC;
}

} // namespace psci
