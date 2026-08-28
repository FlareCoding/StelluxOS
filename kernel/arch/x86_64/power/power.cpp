#include "power/power.h"
#include "acpi/acpi.h"
#include "hw/portio.h"
#include "hw/cpu.h"
#include "hw/delay.h"
#include "hw/mmio.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "trap/idt.h"
#include "common/logging.h"

namespace power {

constexpr uint16_t KBC_CMD_PORT        = 0x64;
constexpr uint8_t  KBC_STATUS_IBF      = 0x02;
constexpr uint8_t  KBC_CMD_PULSE_RESET = 0xFE;
constexpr uint32_t KBC_DRAIN_ATTEMPTS  = 500;

constexpr uint16_t RESET_CTRL_PORT = 0xCF9;
constexpr uint8_t  RESET_CTRL_ARM  = 0x02;  // Select CPU reset without power cycle
constexpr uint8_t  RESET_CTRL_FIRE = 0x06;  // RST_CPU + SYS_RST, triggers the reset

constexpr uint64_t MECHANISM_WAIT_US = 50000;

/**
 * Reset through the FADT reset register, the mechanism the firmware
 * declares for the board. Absent or unusable registers are skipped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void acpi_reset() {
    const acpi::fadt* fadt = acpi::get_fadt();
    if (!fadt || !FADT_HAS_FIELD(fadt, reset_value)) {
        return;
    }

    if (!(fadt->flags & acpi::FADT_FLAG_RESET_REG_SUP) ||
        fadt->reset_reg.address == 0) {
        return;
    }

    switch (fadt->reset_reg.space) {
    case acpi::GAS_SPACE_SYSTEM_IO:
        portio::out8(static_cast<uint16_t>(fadt->reset_reg.address),
                     fadt->reset_value);
        break;
    case acpi::GAS_SPACE_SYSTEM_MEMORY: {
        uintptr_t base = 0;
        uintptr_t va = 0;
        int32_t rc = vmm::map_phys(
            static_cast<pmm::phys_addr_t>(fadt->reset_reg.address), 1,
            paging::PAGE_KERNEL_RW | paging::PAGE_DEVICE, base, va);

        if (rc == vmm::OK) {
            mmio::write8(va, fadt->reset_value);
        }
        break;
    }
    default:
        log::warn("power: unhandled reset register space %u",
                  static_cast<uint32_t>(fadt->reset_reg.space));
        break;
    }
}

/**
 * Pulse the CPU reset line through the keyboard controller.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void kbc_reset() {
    for (uint32_t i = 0; i < KBC_DRAIN_ATTEMPTS; i++) {
        if ((portio::in8(KBC_CMD_PORT) & KBC_STATUS_IBF) == 0) {
            break;
        }

        delay::us(10);
    }

    portio::out8(KBC_CMD_PORT, KBC_CMD_PULSE_RESET);
}

/**
 * Load an empty IDT and trap. The fault cannot be delivered, which
 * escalates to a triple fault and resets the CPU.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void triple_fault() {
    x86::idt_ptr null_idt = {0, 0};
    asm volatile("lidt %0; int3" :: "m"(null_idt));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t shutdown() {
    // Powering off requires the _S5 sleep type values from the DSDT.
    return ERR_UNSUPPORTED;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t reboot() {
    cpu::irq_disable();

    acpi_reset();
    delay::us(MECHANISM_WAIT_US);

    kbc_reset();
    delay::us(MECHANISM_WAIT_US);

    portio::out8(RESET_CTRL_PORT, RESET_CTRL_ARM);
    portio::out8(RESET_CTRL_PORT, RESET_CTRL_FIRE);
    delay::us(MECHANISM_WAIT_US);

    triple_fault();
    return ERR_UNSUPPORTED;
}

} // namespace power
