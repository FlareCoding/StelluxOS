#ifndef TIME_H
#define TIME_H
#include <types.h>

/**
 * @class kernel_timer
 * @brief Provides high-resolution timing and system time functions.
 * 
 * Manages and calibrates system timers, including the HPET (High Precision Event Timer)
 * and APIC (Advanced Programmable Interrupt Controller) timer. Also provides utility 
 * methods to retrieve system time in various units.
 */
class kernel_timer {
public:
    /**
     * @brief Initializes the HPET timer.
     * 
     * Configures and enables the High Precision Event Timer (HPET) to provide 
     * high-resolution timekeeping for the system.
     */
    static void init();

    /**
     * @brief Calibrates the APIC timer based on a given time interval.
     * @param milliseconds The time interval in milliseconds for calibration.
     * 
     * Determines the number of APIC timer ticks per millisecond and stores the
     * calibrated frequency for future use.
     */
    static void calibrate_cpu_timer(uint64_t milliseconds);

    /**
     * @brief Starts the APIC timer in periodic mode.
     * 
     * Configures the APIC timer to generate periodic interrupts based on the
     * calibrated frequency. This is used for scheduling and other periodic tasks.
     */
    static void start_cpu_periodic_timer();

    /**
     * @brief Retrieves the current system time in raw HPET counter ticks.
     * @return The current system time as a raw HPET counter value.
     */
    static uint64_t get_high_precision_system_time();

    /**
     * @brief Retrieves the current system time in nanoseconds.
     * @return The current system time in nanoseconds.
     */
    static uint64_t get_system_time_in_nanoseconds();

    /**
     * @brief Retrieves the current system time in microseconds.
     * @return The current system time in microseconds.
     */
    static uint64_t get_system_time_in_microseconds();

    /**
     * @brief Retrieves the current system time in milliseconds.
     * @return The current system time in milliseconds.
     */
    static uint64_t get_system_time_in_milliseconds();

    /**
     * @brief Retrieves the current system time in seconds.
     * @return The current system time in seconds.
     */
    static uint64_t get_system_time_in_seconds();

    /**
     * @brief Ticks and updates the global system time management system.
     */
    static void sched_irq_global_tick();

private:
    static uint64_t s_apic_ticks_calibrated_frequency;
    static uint64_t s_tsc_ticks_calibrated_frequency;
    static uint64_t s_global_system_time_ns;
    static uint64_t s_configured_apic_interval_ms;
};

/**
 * @brief Reads the current value of the Time Stamp Counter (TSC).
 * @return The current TSC value.
 * 
 * Reads the 64-bit value of the CPU's Time Stamp Counter, which increments on
 * each CPU cycle. Useful for high-resolution timing.
 */
__force_inline__ uint64_t rdtsc() {
    uint32_t hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

/**
 * @brief Sleeps for the specified number of seconds.
 * @param seconds The duration to sleep, in seconds.
 * 
 * Currently uses a busy-wait implementation.
 */
void sleep(uint32_t seconds);

/**
 * @brief Sleeps for the specified number of milliseconds.
 * @param milliseconds The duration to sleep, in milliseconds.
 * 
 * Currently uses a busy-wait implementation.
 */
void msleep(uint32_t milliseconds);

/**
 * @brief Sleeps for the specified number of microseconds.
 * @param microseconds The duration to sleep, in microseconds.
 * 
 * Currently uses a busy-wait implementation.
 */
void usleep(uint32_t microseconds);

/**
 * @brief Sleeps for the specified number of nanoseconds.
 * @param nanoseconds The duration to sleep, in nanoseconds.
 * 
 * Currently uses a busy-wait implementation.
 */
void nanosleep(uint32_t nanoseconds);

#endif // TIME_H
