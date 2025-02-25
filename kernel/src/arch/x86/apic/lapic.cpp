#ifdef ARCH_X86_64
#include <arch/x86/apic/lapic.h>
#include <arch/percpu.h>
#include <sched/sched.h>
#include <ports/ports.h>
#include <memory/vmm.h>
#include <memory/paging.h>

// ICR Register Offsets in the LAPIC MMIO space
static constexpr uint32_t APIC_REG_ICR_LOW   = 0x300; // Interrupt Command Register [31:0]
static constexpr uint32_t APIC_REG_ICR_HIGH  = 0x310; // Interrupt Command Register [63:32]

// Bits in ICR_LOW (32 bits)
static constexpr uint32_t APIC_VECTOR_MASK         = 0x000000FF;  // Bits [7:0]:  Interrupt vector
// Delivery Mode (bits [10:8]) 
static constexpr uint32_t APIC_DM_FIXED            = (0 << 8);    // 000: Fixed
static constexpr uint32_t APIC_DM_LOWEST           = (1 << 8);    // 001: Lowest Priority
static constexpr uint32_t APIC_DM_SMI              = (2 << 8);    // 010: SMI (system management interrupt)
static constexpr uint32_t APIC_DM_NMI              = (4 << 8);    // 100: NMI
static constexpr uint32_t APIC_DM_INIT             = (5 << 8);    // 101: INIT
static constexpr uint32_t APIC_DM_STARTUP          = (6 << 8);    // 110: STARTUP
// Destination Mode (bit 11)
static constexpr uint32_t APIC_DESTMODE_LOGICAL    = (1 << 11);   // 1 for logical, 0 for physical
// Delivery Status (bit 12)
static constexpr uint32_t APIC_DELIVERY_STATUS     = (1 << 12);   // 1 if APIC is busy sending
// Level (bit 14) - used only for INIT level de-assert
static constexpr uint32_t APIC_LEVEL_ASSERT        = (1 << 14);  
static constexpr uint32_t APIC_LEVEL_DEASSERT      = (0 << 14); // same bit = 0
// Trigger Mode (bit 15)
static constexpr uint32_t APIC_TRIGGER_LEVEL       = (1 << 15);   // 1 for level, 0 for edge
static constexpr uint32_t APIC_TRIGGER_EDGE        = (0 << 15);   
// Destination Shorthand (bits [19:18])
//  00 = No shorthand, 01 = Self, 02 = All including self, 03 = All excluding self
// Usually we leave it at 00 because we specify the APIC ID in ICR_HIGH.

// Bits in ICR_HIGH (32 bits)
static constexpr uint32_t APIC_ICR_DEST_SHIFT      = 24;          // Bits [31:24]: Destination APIC ID

namespace arch::x86 {
__PRIVILEGED_DATA
kstl::shared_ptr<lapic> s_system_lapics[MAX_SYSTEM_CPUS];

__PRIVILEGED_DATA
uintptr_t g_lapic_physical_base = 0;

__PRIVILEGED_DATA
volatile uint32_t* g_lapic_virtual_base = 0;

lapic::lapic(uint64_t base, uint8_t spurious_irq) {
    //
    // Since every core's LAPIC shares the same base address in
    // terms of memory mapping, the page mapping call needs to
    // only happen once.
    //
    if (!g_lapic_physical_base) {
        g_lapic_physical_base = base;

        // Map the LAPIC base into the kernel's address space
        void* virt_base = vmm::map_physical_page(base, DEFAULT_PRIV_PAGE_FLAGS | PTE_PCD);

        // Update the globally tracked virtual base pointer
        g_lapic_virtual_base = reinterpret_cast<volatile uint32_t*>(virt_base);
    }

    // Set the spurious interrupt vector
    uint32_t spurious_vector = read(0xF0);
    spurious_vector |= (1 << 8);        // Enable the APIC
    spurious_vector |= spurious_irq;    // Set the spurious interrupt vector (choose a free vector number)
    write(0xF0, spurious_vector);

    // Disable legacy PIC controller
    disable_legacy_pic();
}

__PRIVILEGED_CODE 
void lapic::write(uint32_t reg, uint32_t value) {
    g_lapic_virtual_base[reg / 4] = value;
}

__PRIVILEGED_CODE 
uint32_t lapic::read(uint32_t reg) {
    return g_lapic_virtual_base[reg / 4];
}

__PRIVILEGED_CODE 
void lapic::mask_irq(uint32_t lvtoff) {
    // Read the current LVT entry
    uint32_t lvt_entry = read(lvtoff);

    // Set the mask bit (bit 16)
    lvt_entry |= (1 << 16);

    // Write the modified LVT entry back
    write(lvtoff, lvt_entry);
}

__PRIVILEGED_CODE 
void lapic::unmask_irq(uint32_t lvtoff) {
    // Read the current LVT entry
    uint32_t lvt_entry = read(lvtoff);

    // Clear the mask bit (bit 16)
    lvt_entry &= ~(1 << 16);

    // Write the modified LVT entry back
    write(lvtoff, lvt_entry);
}

__PRIVILEGED_CODE 
void lapic::mask_timer_irq() {
    mask_irq(APIC_LVT_TIMER);
}

__PRIVILEGED_CODE 
void lapic::unmask_timer_irq() {
    unmask_irq(APIC_LVT_TIMER);
}

__PRIVILEGED_CODE 
void lapic::complete_irq() {
    write(0xB0, 0x00);
}

__PRIVILEGED_CODE 
void lapic::send_init_ipi(uint8_t apic_id) {
    // ------------------------------------------------------
    // Write to ICR_HIGH: set the target APIC ID
    // ------------------------------------------------------
    //
    // Bits [31:24] = Destination APIC ID
    // The lower 24 bits of ICR_HIGH are reserved (must be 0).
    // 
    write(APIC_REG_ICR_HIGH, static_cast<uint32_t>(apic_id) << APIC_ICR_DEST_SHIFT);

    // ------------------------------------------------------
    // Write to ICR_LOW: set the command for INIT IPI
    // ------------------------------------------------------
    //
    // For an INIT IPI:
    //   - Vector = 0 (bits [7:0] = 0)
    //   - Delivery Mode = 101b (INIT)
    //   - Level = 1 (Assert) 
    //   - Trigger Mode = 1 (Level)
    //   - Destination Mode = 0 (Physical, if you want physical APIC ID)
    //   - Destination Shorthand = 00 (bits [19:18] = 0)
    //
    // Putting it all together:
    //   ICR_LOW = APIC_DM_INIT | APIC_TRIGGER_LEVEL | APIC_LEVEL_ASSERT
    //
    uint32_t icr_low = APIC_DM_INIT | APIC_TRIGGER_LEVEL | APIC_LEVEL_ASSERT;
    write(APIC_REG_ICR_LOW, icr_low);

    // ------------------------------------------------------
    // Wait for the send to complete
    // ------------------------------------------------------
    wait_for_icr_cmd_completion();

    // ------------------------------------------------------
    // De-assert the INIT IPI (per Intel specs),
    // done by writing the same command but with
    // Level=0 (deassert).
    // ------------------------------------------------------
    write(APIC_REG_ICR_HIGH, (uint32_t)apic_id << APIC_ICR_DEST_SHIFT);

    // APIC_DM_INIT | APIC_TRIGGER_LEVEL is still set, but now Level=0
    icr_low = APIC_DM_INIT | APIC_TRIGGER_LEVEL | APIC_LEVEL_DEASSERT;
    write(APIC_REG_ICR_LOW, icr_low);

    // Wait again for send to complete
    wait_for_icr_cmd_completion();
}

__PRIVILEGED_CODE 
void lapic::send_startup_ipi(uint8_t apic_id, uint32_t vector) {
    // ------------------------------------------------------
    // Write ICR_HIGH: set the target APIC ID
    // ------------------------------------------------------
    write(APIC_REG_ICR_HIGH, static_cast<uint32_t>(apic_id) << APIC_ICR_DEST_SHIFT);

    // ------------------------------------------------------
    // Write ICR_LOW: set command for STARTUP IPI
    // ------------------------------------------------------
    //
    // For a STARTUP IPI:
    //   - Vector = startup_vector (bits [7:0])
    //   - Delivery Mode = 110b (STARTUP)
    //   - Trigger Mode = 0 (Edge)
    //   - Level = 1 (Assert) — but for STARTUP, we treat it as edge,
    //       so bit 15 = 0 for edge, bit 14 can be 0 or 1. Typically 0 is used 
    //       in practice (Intel’s examples vary but edge triggers do not rely on level).
    //
    // The CPU will start execution at physical address = startup_vector * 0x1000.
    //
    // So:
    //    ICR_LOW = (startup_vector & APIC_VECTOR_MASK)
    //               | APIC_DM_STARTUP
    //               | APIC_TRIGGER_EDGE
    //               | ...
    //
    uint32_t icr_low = (static_cast<uint32_t>(vector) & APIC_VECTOR_MASK)
                     | APIC_DM_STARTUP
                     | APIC_TRIGGER_EDGE;

    write(APIC_REG_ICR_LOW, icr_low);

    // ------------------------------------------------------
    // Wait for send to complete
    // ------------------------------------------------------
    wait_for_icr_cmd_completion();
}

__PRIVILEGED_CODE void lapic::wait_for_icr_cmd_completion() {
    // Wait until the Delivery Status bit is cleared, meaning
    // the IPI has been sent (the hardware is not busy anymore).
    while (g_lapic_virtual_base[APIC_REG_ICR_LOW / 4] & APIC_DELIVERY_STATUS) {
        asm volatile("pause");
    }
}

__PRIVILEGED_CODE 
void lapic::init() {
    int cpu = current->cpu;

    if (s_system_lapics[cpu].get() != nullptr) {
        return;
    }

    uint64_t apic_base_msr = msr::read(IA32_APIC_BASE_MSR);

    // Enable APIC by setting the 11th bit
    apic_base_msr |= (1 << 11);

    msr::write(IA32_APIC_BASE_MSR, apic_base_msr);

    uint64_t physical_base = reinterpret_cast<uint64_t>(apic_base_msr & ~0xFFF);
    s_system_lapics[cpu] = kstl::make_shared<lapic>(physical_base, 0xFF);
}

__PRIVILEGED_CODE 
kstl::shared_ptr<lapic>& lapic::get() {
    int cpu = current->cpu;

    if (s_system_lapics[cpu].get() != nullptr) {
        return s_system_lapics[cpu];
    }

    init();
    return s_system_lapics[cpu];
}

__PRIVILEGED_CODE
kstl::shared_ptr<lapic>& lapic::get(int cpu) {
    return s_system_lapics[cpu];
}

__PRIVILEGED_CODE 
void lapic::disable_legacy_pic() {
    // Send the disable command (0xFF) to both PIC1 and PIC2 data ports
    outb(0xA1, 0xFF);
    outb(0x21, 0xFF);
}
} // namespace arch::x86

#endif // ARCH_X86_64
