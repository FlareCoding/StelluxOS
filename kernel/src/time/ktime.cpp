#include "ktime.h"
#include <acpi/acpi_controller.h>
#include <arch/x86/apic_timer.h>
#include <kelevate/kelevate.h>
#include <paging/tlb.h>
#include <arch/x86/per_cpu_data.h>
#include <sched/sched.h>

Hpet* g_precisionTimerInstance = nullptr;
uint64_t g_hardwareFrequency = 0;
uint64_t KernelTimer::s_apicTicksCalibratedFrequency = 0;
uint64_t KernelTimer::s_tscTicksCalibratedFrequency = 0;

void KernelTimer::init() {
    auto& acpiController = AcpiController::get();
    g_precisionTimerInstance = acpiController.getHpet();

    g_precisionTimerInstance->init();
    g_hardwareFrequency = g_precisionTimerInstance->qeueryFrequency();

    // TLB has to be flushed for proper writes to HPET registers in the future
    RUN_ELEVATED({
        paging::flushTlbAll();
    });
}

void KernelTimer::calibrateApicTimer(uint64_t milliseconds) {
    auto& apicTimer = ApicTimer::get();

    RUN_ELEVATED(disableInterrupts(););

    apicTimer.setupOneShot(IRQ0, 1, 0xffffffff);

    // Record the start time from HPET and APIC
    uint64_t hpetStart = g_precisionTimerInstance->readCounter();
    uint64_t rdtscStart = rdtsc();
    apicTimer.start();

    // Wait for 1 second
    while (g_precisionTimerInstance->readCounter() - hpetStart < g_hardwareFrequency) {
        asm volatile ("nop");
    }

    // Stop the APIC timer and get the current count
    uint32_t apicEnd = apicTimer.stop();
    uint64_t rdtscEnd = rdtsc();

    RUN_ELEVATED(enableInterrupts(););

    // Calculate the number of elapsed APIC ticks
    // Assuming APIC timer counts down from the initial count
    s_apicTicksCalibratedFrequency = (((uint64_t)(0xffffffff - apicEnd)) / 1000) * milliseconds;
    s_tscTicksCalibratedFrequency = rdtscEnd - rdtscStart;
}

void KernelTimer::startApicPeriodicTimer() {
    auto& apicTimer = ApicTimer::get();
    apicTimer.setupPeriodic(IRQ0, 1, s_apicTicksCalibratedFrequency);
    apicTimer.start();
}

uint64_t KernelTimer::getSystemTime() {
    return g_precisionTimerInstance->readCounter();
}

uint64_t KernelTimer::getSystemTimeInNanoseconds() {
    return (getSystemTime() / g_hardwareFrequency) * 1000000000ULL;
}

uint64_t KernelTimer::getSystemTimeInMicroseconds() {
    return (getSystemTime() / g_hardwareFrequency) * 1000000ULL;
}

uint64_t KernelTimer::getSystemTimeInMilliseconds() {
    return (getSystemTime() / g_hardwareFrequency) * 1000ULL;
}

uint64_t KernelTimer::getSystemTimeInSeconds() {
    return getSystemTime() / g_hardwareFrequency;
}

void sleep(uint32_t seconds) {
    uint64_t start = g_precisionTimerInstance->readCounter();

    while (g_precisionTimerInstance->readCounter() < start + (seconds * g_hardwareFrequency)) {
        yield();
    }
}

void msleep(uint32_t milliseconds) {
    uint64_t start = g_precisionTimerInstance->readCounter();

    while (
        g_precisionTimerInstance->readCounter() <
        start + (milliseconds * (g_hardwareFrequency / 1000ULL))
    ) {
        yield();
    }
}

void usleep(uint32_t microseconds) {
    uint64_t start = g_precisionTimerInstance->readCounter();

    while (
        g_precisionTimerInstance->readCounter() <
        start + (microseconds * (g_hardwareFrequency / 1000000ULL))
    ) {
        yield();
    }
}

void nanosleep(uint32_t nanoseconds) {
    uint64_t start = g_precisionTimerInstance->readCounter();

    while (
        g_precisionTimerInstance->readCounter() <
        start + (nanoseconds * (g_hardwareFrequency / 1000000000ULL))
    ) {
        yield();
    }
}
