#ifdef ARCH_X86_64
#ifndef LAPIC_H
#define LAPIC_H
#include <memory/memory.h>
#include <arch/x86/msr.h>

#define IA32_APIC_BASE_MSR          0x1B
#define APIC_REGISTER_SPACE_SIZE    0x400

// The offset of the ICR (Interrupt Command Register) in the Local APIC
#define APIC_ICR_LO 0x300  
#define APIC_ICR_HI 0x310

#define APIC_LVT_TIMER    0x320  // Timer interrupt
#define APIC_LVT_THERMAL  0x330  // Thermal sensor interrupt
#define APIC_LVT_PERF     0x340  // Performance monitoring interrupt
#define APIC_LVT_LINT0    0x350  // LINT0 interrupt
#define APIC_LVT_LINT1    0x360  // LINT1 interrupt
#define APIC_LVT_ERROR    0x370  // Error interrupt

namespace arch::x86 {
/**
 * @class lapic
 * @brief Manages the Local Advanced Programmable Interrupt Controller (LAPIC).
 * 
 * This class provides functionality for initializing, configuring, and controlling the LAPIC, including
 * interrupt management and inter-processor communication.
 */
class lapic {
public:
    /**
     * @brief Initializes the LAPIC.
     * 
     * Sets up the LAPIC for operation, including configuring its registers for interrupt handling.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void init();

    /**
     * @brief Retrieves the singleton instance of the LAPIC.
     * @return A shared pointer to the singleton lapic instance.
     * 
     * Provides access to the LAPIC instance for the current CPU.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static kstl::shared_ptr<lapic>& get();

    /**
     * @brief Retrieves the LAPIC instance for a specific CPU.
     * @param cpu The CPU ID for which to retrieve the LAPIC instance.
     * @return A shared pointer to the LAPIC instance for the specified CPU.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static kstl::shared_ptr<lapic>& get(int cpu);

    /**
     * @brief Constructs a LAPIC instance.
     * @param base The physical base address of the LAPIC registers.
     * @param spurior_irq The spurious interrupt vector (default: 0xFF).
     * 
     * This constructor initializes a LAPIC instance with the specified base address and spurious IRQ vector.
     */
    lapic(uint64_t base, uint8_t spurior_irq = 0xFF);

    /**
     * @brief Writes a value to a specified LAPIC register.
     * @param reg The register offset.
     * @param value The value to write.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void write(uint32_t reg, uint32_t value);

    /**
     * @brief Reads the value from a specified LAPIC register.
     * @param reg The register offset.
     * @return The value of the specified LAPIC register.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint32_t read(uint32_t reg);

    /**
     * @brief Masks the IRQ associated with a specific Local Vector Table (LVT) offset.
     * @param lvtoff The LVT offset to mask.
     * 
     * Prevents the specified LVT IRQ from being delivered.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void mask_irq(uint32_t lvtoff);

    /**
     * @brief Unmasks the IRQ associated with a specific Local Vector Table (LVT) offset.
     * @param lvtoff The LVT offset to unmask.
     * 
     * Allows the specified LVT IRQ to be delivered.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unmask_irq(uint32_t lvtoff);

    /**
     * @brief Masks the LAPIC timer IRQ.
     * 
     * Prevents timer interrupts from being delivered by the LAPIC.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void mask_timer_irq();

    /**
     * @brief Unmasks the LAPIC timer IRQ.
     * 
     * Allows timer interrupts to be delivered by the LAPIC.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unmask_timer_irq();

    /**
     * @brief Signals completion of the current interrupt.
     * 
     * Notifies the LAPIC that the current interrupt has been handled, allowing further interrupts to be delivered.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void complete_irq();

    /**
     * @brief Sends an INIT Inter-Processor Interrupt (IPI) to a specified LAPIC.
     * @param apic_id The APIC ID of the target LAPIC.
     * 
     * Used for inter-processor communication to trigger an INIT action on other processors.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void send_init_ipi(uint8_t apic_id);

    /**
     * @brief Sends a Startup Inter-Processor Interrupt (IPI) to a specified LAPIC.
     * @param apic_id The APIC ID of the target LAPIC.
     * @param vector The interrupt vector to send.
     * 
     * Used for inter-processor communication to trigger a Startup action on other processors.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void send_startup_ipi(uint8_t apic_id, uint32_t vector);

    /**
     * @brief Waits for an ICR command completion by reading the Delivery Status bit.
     * 
     * Used in inter-processor communication through IPIs.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void wait_for_icr_cmd_completion();

    /**
     * @brief Disables the legacy Programmable Interrupt Controller (PIC).
     * 
     * Ensures the LAPIC is the primary interrupt controller by disabling the legacy PIC.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void disable_legacy_pic();
};
} // namespace arch::x86

#endif // LAPIC_H
#endif // ARCH_X86_64
