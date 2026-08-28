#include "power/power.h"
#include "power/s5.h"
#include "acpi/acpi.h"
#include "hw/portio.h"
#include "hw/cpu.h"
#include "hw/delay.h"
#include "hw/mmio.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "trap/idt.h"
#include "common/logging.h"
#include "common/string.h"

namespace power {

// Resolved PM1 control register, either an I/O port or mapped MMIO
struct pm1_block {
    bool valid;
    bool is_io;
    uint16_t port;
    uintptr_t va;
};

constexpr uint16_t KBC_CMD_PORT        = 0x64;
constexpr uint8_t  KBC_STATUS_IBF      = 0x02;
constexpr uint8_t  KBC_CMD_PULSE_RESET = 0xFE;
constexpr uint32_t KBC_DRAIN_ATTEMPTS  = 500;

constexpr uint16_t RESET_CTRL_PORT = 0xCF9;
constexpr uint8_t  RESET_CTRL_ARM  = 0x02;  // Select CPU reset without power cycle
constexpr uint8_t  RESET_CTRL_FIRE = 0x06;  // RST_CPU + SYS_RST, triggers the reset

constexpr uint64_t MECHANISM_WAIT_US = 50000;

// PM1 control register bits
constexpr uint16_t PM1_SCI_EN       = (1 << 0);
constexpr uint16_t PM1_SLP_EN       = (1 << 13);
constexpr uint16_t PM1_SLP_TYP_MASK = (0x7 << 10);
constexpr uint32_t PM1_SLP_TYP_SHIFT = 10;

constexpr uint32_t SCI_EN_POLL_ATTEMPTS = 10000;
constexpr uint64_t SCI_EN_POLL_DELAY_US = 100;

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
 * Map the DSDT through the FADT pointer and extract the \_S5 values.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool lookup_s5_sleep_types(const acpi::fadt* fadt,
                                                    uint8_t* typ_a,
                                                    uint8_t* typ_b) {
    uint64_t dsdt_phys = fadt->dsdt;
    if (FADT_HAS_FIELD(fadt, x_dsdt) && fadt->x_dsdt != 0) {
        dsdt_phys = fadt->x_dsdt;
    }

    const acpi::sdt_header* dsdt = acpi::map_table(dsdt_phys);
    if (!dsdt || string::memcmp(dsdt->signature, "DSDT", 4) != 0) {
        return false;
    }

    const auto* aml = reinterpret_cast<const uint8_t*>(dsdt)
                    + sizeof(acpi::sdt_header);
    size_t aml_len = dsdt->length - sizeof(acpi::sdt_header);

    return find_s5_sleep_types(aml, aml_len, typ_a, typ_b);
}

/**
 * Resolve a PM1 control block, preferring the extended GAS form over
 * the legacy port field when the FADT provides a usable one.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static pm1_block resolve_pm1_cnt(
    const acpi::generic_address* xreg, uint32_t legacy_port
) {
    pm1_block blk = {};

    if (xreg && xreg->address != 0) {
        if (xreg->space == acpi::GAS_SPACE_SYSTEM_IO) {
            blk.valid = true;
            blk.is_io = true;
            blk.port = static_cast<uint16_t>(xreg->address);
            return blk;
        }

        if (xreg->space == acpi::GAS_SPACE_SYSTEM_MEMORY) {
            uintptr_t base = 0;
            uintptr_t va = 0;
            int32_t rc = vmm::map_phys(
                static_cast<pmm::phys_addr_t>(xreg->address), sizeof(uint16_t),
                paging::PAGE_KERNEL_RW | paging::PAGE_DEVICE, base, va);
            if (rc == vmm::OK) {
                blk.valid = true;
                blk.va = va;
                return blk;
            }
        }
    }

    if (legacy_port != 0) {
        blk.valid = true;
        blk.is_io = true;
        blk.port = static_cast<uint16_t>(legacy_port);
    }

    return blk;
}

/** @note Privilege: **required** */
__PRIVILEGED_CODE static uint16_t pm1_read(const pm1_block& blk) {
    return blk.is_io ? portio::in16(blk.port) : mmio::read16(blk.va);
}

/** @note Privilege: **required** */
__PRIVILEGED_CODE static void pm1_write(const pm1_block& blk, uint16_t val) {
    if (blk.is_io) {
        portio::out16(blk.port, val);
    } else {
        mmio::write16(blk.va, val);
    }
}

/**
 * Put the chipset into ACPI mode if firmware left it in legacy mode.
 * The PM1 sleep bits have no effect until SCI_EN is set.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void enable_acpi_mode(const acpi::fadt* fadt,
                                               const pm1_block& pm1a) {
    if (pm1_read(pm1a) & PM1_SCI_EN) {
        return;
    }

    if (fadt->smi_cmd == 0 || fadt->acpi_enable == 0) {
        return;
    }

    portio::out8(static_cast<uint16_t>(fadt->smi_cmd), fadt->acpi_enable);

    for (uint32_t i = 0; i < SCI_EN_POLL_ATTEMPTS; i++) {
        if (pm1_read(pm1a) & PM1_SCI_EN) {
            return;
        }
        delay::us(SCI_EN_POLL_DELAY_US);
    }

    log::warn("power: SCI_EN did not latch after the ACPI enable command");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t shutdown() {
    const acpi::fadt* fadt = acpi::get_fadt();
    if (!fadt) {
        log::warn("power: no FADT, cannot power off");
        return ERR_UNSUPPORTED;
    }

    uint8_t typ_a = 0;
    uint8_t typ_b = 0;
    if (!lookup_s5_sleep_types(fadt, &typ_a, &typ_b)) {
        log::warn("power: _S5 sleep values not found, cannot power off");
        return ERR_UNSUPPORTED;
    }

    pm1_block pm1a = resolve_pm1_cnt(
        FADT_HAS_FIELD(fadt, x_pm1a_cnt_blk) ? &fadt->x_pm1a_cnt_blk : nullptr,
        fadt->pm1a_cnt_blk);
    if (!pm1a.valid) {
        log::warn("power: PM1a control block unavailable");
        return ERR_UNSUPPORTED;
    }

    pm1_block pm1b = resolve_pm1_cnt(
        FADT_HAS_FIELD(fadt, x_pm1b_cnt_blk) ? &fadt->x_pm1b_cnt_blk : nullptr,
        fadt->pm1b_cnt_blk);

    cpu::irq_disable();
    enable_acpi_mode(fadt, pm1a);

    // Enter S5: write SLP_TYP with SLP_EN to PM1a, then PM1b if present
    uint16_t val = pm1_read(pm1a);
    val &= static_cast<uint16_t>(~(PM1_SLP_TYP_MASK | PM1_SLP_EN));
    pm1_write(pm1a, val
        | static_cast<uint16_t>(typ_a << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN);

    if (pm1b.valid) {
        val = pm1_read(pm1b);
        val &= static_cast<uint16_t>(~(PM1_SLP_TYP_MASK | PM1_SLP_EN));
        pm1_write(pm1b, val
            | static_cast<uint16_t>(typ_b << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN);
    }

    delay::us(MECHANISM_WAIT_US);

    log::warn("power: ACPI S5 entry had no effect");
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
