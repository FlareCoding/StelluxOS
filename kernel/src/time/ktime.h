#ifndef KTIME_H
#define KTIME_H
#include <ktypes.h>

class KernelTimer {
public:
    // Initializes HPET timer
    static void init();

    // Calibrates APIC timer to the number of ticks per given number of milliseconds
    static void calibrateApicTimer(uint64_t milliseconds);

    // Starts the interrupt driven APIC periodic timer
    static void startApicPeriodicTimer();

    // Reads the CPU timestamp counter
    static uint64_t rdtsc();

    // Reads HPET time counter value
    static uint64_t getSystemTime();
    static uint64_t getSystemTimeInNanoseconds();
    static uint64_t getSystemTimeInMicroseconds();
    static uint64_t getSystemTimeInMilliseconds();
    static uint64_t getSystemTimeInSeconds();

private:
    static uint64_t s_apicTicksCalibratedFrequency;
    static uint64_t s_tscTicksCalibratedFrequency;
};

void sleep(uint32_t seconds);

void msleep(uint32_t milliseconds);

void usleep(uint32_t microseconds);

void nanosleep(uint32_t nanoseconds);

#endif