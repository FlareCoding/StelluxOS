#include "irq/irq.h"
#include "irq/lapic.h"
#include "irq/ioapic.h"
#include "defs/vectors.h"
#include "hw/portio.h"
#include "common/logging.h"

namespace irq {

/**
 * Mask both 8259 PICs to prevent spurious legacy interrupts.
 * Harmless if the PIC is not present.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void mask_legacy_pic() {
    portio::out8(0x21, 0xFF); // master PIC data
    portio::out8(0xA1, 0xFF); // slave PIC data
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    mask_legacy_pic();

    int32_t rc = init_lapic();
    if (rc != OK) {
        return rc;
    }

    // Mask all LVT entries to clear any stale vectors left by UEFI firmware
    write_lapic_register(LAPIC_LVT_TIMER,   LVT_MASKED);
    write_lapic_register(LAPIC_LVT_THERMAL, LVT_MASKED);
    write_lapic_register(LAPIC_LVT_PERFCNT, LVT_MASKED);
    write_lapic_register(LAPIC_LVT_LINT0,   LVT_MASKED);
    write_lapic_register(LAPIC_LVT_LINT1,   LVT_MASKED);
    write_lapic_register(LAPIC_LVT_ERROR,   LVT_MASKED);

    // Enable LAPIC: preserve reserved SVR bits, set enable + spurious vector
    uint32_t svr = read_lapic_register(LAPIC_SVR);
    svr = (svr & ~static_cast<uint32_t>(0xFF)) | 0x1FF;
    write_lapic_register(LAPIC_SVR, svr);

    // Clear any stale interrupt state
    write_lapic_register(LAPIC_EOI, 0);

    log::info("irq: LAPIC enabled (spurious=0x%02x)",
              static_cast<uint32_t>(x86::VEC_SPURIOUS));

    int32_t ioapic_rc = ioapic::init();
    if (ioapic_rc != ioapic::OK && ioapic_rc != ioapic::ERR_NONE) {
        log::error("irq: IOAPIC init failed");
        return ERR_MAP;
    }

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void eoi(uint32_t) {
    write_lapic_register(LAPIC_EOI, 0);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmask(uint32_t irq) {
    if (irq == x86::VEC_TIMER) {
        uint32_t lvt = read_lapic_register(LAPIC_LVT_TIMER);
        lvt &= ~LVT_MASKED;
        write_lapic_register(LAPIC_LVT_TIMER, lvt);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mask(uint32_t irq) {
    if (irq == x86::VEC_TIMER) {
        uint32_t lvt = read_lapic_register(LAPIC_LVT_TIMER);
        lvt |= LVT_MASKED;
        write_lapic_register(LAPIC_LVT_TIMER, lvt);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap() {
    write_lapic_register(LAPIC_LVT_TIMER,   LVT_MASKED);
    write_lapic_register(LAPIC_LVT_THERMAL, LVT_MASKED);
    write_lapic_register(LAPIC_LVT_PERFCNT, LVT_MASKED);
    write_lapic_register(LAPIC_LVT_LINT0,   LVT_MASKED);
    write_lapic_register(LAPIC_LVT_LINT1,   LVT_MASKED);
    write_lapic_register(LAPIC_LVT_ERROR,   LVT_MASKED);

    uint32_t svr = read_lapic_register(LAPIC_SVR);
    svr = (svr & ~static_cast<uint32_t>(0xFF)) | 0x1FF;
    write_lapic_register(LAPIC_SVR, svr);

    write_lapic_register(LAPIC_EOI, 0);

    return OK;
}

} // namespace irq
