#ifndef TIME_H
#define TIME_H
#include <types.h>

class kernel_timer {
public:
    // Initializes HPET timer
    static void init();

    // Calibrates APIC timer to the number of ticks per given number of milliseconds
    static void calibrate_cpu_timer(uint64_t milliseconds);

    // Starts the interrupt driven APIC periodic timer
    static void start_cpu_periodic_timer();

    // Reads HPET time counter value
    static uint64_t get_system_time();
    static uint64_t get_system_time_in_nanoseconds();
    static uint64_t get_system_time_in_microseconds();
    static uint64_t get_system_time_in_milliseconds();
    static uint64_t get_system_time_in_seconds();

private:
    static uint64_t s_apic_ticks_calibrated_frequency;
    static uint64_t s_tsc_ticks_calibrated_frequency;
};

__force_inline__ uint64_t rdtsc() {
    uint32_t hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

void sleep(uint32_t seconds);

void msleep(uint32_t milliseconds);

void usleep(uint32_t microseconds);

void nanosleep(uint32_t nanoseconds);

#endif // TIME_H
