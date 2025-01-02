#ifdef ARCH_X86_64
#ifndef LAPIC_TIMER_H
#define LAPIC_TIMER_H
#include "lapic.h"

// Macros for APIC Timer Registers and Configurations
#define APIC_TIMER_REGISTER        0x320
#define APIC_TIMER_DIVIDE_CONFIG   0x3E0
#define APIC_TIMER_INITIAL_COUNT   0x380
#define APIC_CURRENT_COUNT         0x390

#define APIC_TIMER_ONE_SHOT_MODE   0x0
#define APIC_TIMER_PERIODIC_MODE   0x20000

namespace arch::x86 {
/**
 * @class apic_timer
 * @brief Manages the Advanced Programmable Interrupt Controller (APIC) timer.
 * 
 * This class provides functionality to configure and control the APIC timer in periodic and one-shot modes.
 */
class apic_timer {
public:
    /**
     * @brief Gets the singleton instance of the apic_timer class.
     * @return The singleton apic_timer instance.
     * 
     * Ensures a single global instance of the APIC timer is used.
     */
    static apic_timer& get();

    /**
     * @brief Default constructor for the apic_timer class.
     */
    apic_timer() = default;

    /**
     * @brief Configures the APIC timer in periodic mode.
     * @param irq_number The IRQ number associated with the APIC timer.
     * @param divide_config The divide configuration for the timer.
     * @param interval_value The interval value for the timer.
     * 
     * Sets up the APIC timer to generate periodic interrupts at the specified interval.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void setup_periodic(uint8_t irq_number, uint32_t divide_config, uint32_t interval_value);

    /**
     * @brief Configures the APIC timer in one-shot mode.
     * @param irq_number The IRQ number associated with the APIC timer.
     * @param divide_config The divide configuration for the timer.
     * @param interval_value The interval value for the timer.
     * 
     * Sets up the APIC timer to generate a single interrupt after the specified interval.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void setup_one_shot(uint8_t irq_number, uint32_t divide_config, uint32_t interval_value);

    /**
     * @brief Starts the APIC timer.
     * 
     * This function starts the configured APIC timer.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void start() const;

    /**
     * @brief Reads the current value of the APIC timer counter.
     * @return The current value of the APIC timer counter.
     * 
     * Provides the current countdown value of the APIC timer.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint32_t read_counter() const;

    /**
     * @brief Stops the APIC timer and retrieves the last counter value.
     * @return The counter value at the moment the timer was stopped.
     * 
     * Halts the APIC timer and returns the final value of the counter.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint32_t stop() const;

private:
    uint8_t     m_irqno;          /** IRQ number associated with the APIC timer */
    uint32_t    m_divide_config;  /** Divide configuration for the timer */
    uint32_t    m_interval_value; /** Interval value for the timer */

    /**
     * @brief Internal setup function for the APIC timer.
     * @param mode The timer mode (periodic or one-shot).
     * @param irq_number The IRQ number associated with the APIC timer.
     * @param divide_config The divide configuration for the timer.
     * @param interval_value The interval value for the timer.
     * 
     * Provides a unified setup mechanism for periodic and one-shot configurations.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void _setup(uint32_t mode, uint8_t irq_number, uint32_t divide_config, uint32_t interval_value);
};
} // namespace arch::x86

#endif // LAPIC_TIMER_H
#endif // ARCH_X86_64
