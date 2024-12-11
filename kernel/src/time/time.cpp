#include <time/time.h>
#include <acpi/hpet.h>
#include <interrupts/irq.h>
#include <arch/x86/apic/apic_timer.h>

uint64_t g_hardware_frequency = 0;
uint64_t kernel_timer::s_apic_ticks_calibrated_frequency = 0;
uint64_t kernel_timer::s_tsc_ticks_calibrated_frequency = 0;

void kernel_timer::init() {
    auto& timer = acpi::hpet::get();

    // Query HPET hardware frequency
    g_hardware_frequency = timer.qeuery_frequency();
}

void kernel_timer::calibrate_cpu_timer(uint64_t milliseconds) {
    auto& apic_timer = arch::x86::apic_timer::get();
    auto& hpet_timer = acpi::hpet::get();

    // Disable timer interrupts
    arch::x86::lapic::get()->mask_timer_irq();

    apic_timer.setup_one_shot(IRQ0, 1, 0xffffffff);

    // Record the start time from HPET and APIC
    uint64_t hpet_start = hpet_timer.read_counter();
    uint64_t rdtsc_start = rdtsc();
    apic_timer.start();

    // Wait for 1 second
    while (hpet_timer.read_counter() - hpet_start < g_hardware_frequency) {
        asm volatile ("nop");
    }

    // Stop the APIC timer and get the current count
    uint32_t apic_end = apic_timer.stop();
    uint64_t rdtsc_end = rdtsc();

    // Re-enable timer interrupts
    arch::x86::lapic::get()->unmask_timer_irq();

    // Calculate the number of elapsed APIC ticks
    // Assuming APIC timer counts down from the initial count
    s_apic_ticks_calibrated_frequency = (((uint64_t)(0xffffffff - apic_end)) / 1000) * milliseconds;
    s_tsc_ticks_calibrated_frequency = rdtsc_end - rdtsc_start;
}

void kernel_timer::start_cpu_periodic_timer() {
    auto& apic_timer = arch::x86::apic_timer::get();

    apic_timer.setup_periodic(IRQ0, 1, s_apic_ticks_calibrated_frequency);
    apic_timer.start();
}

uint64_t kernel_timer::get_system_time() {
    return acpi::hpet::get().read_counter();
}

uint64_t kernel_timer::get_system_time_in_nanoseconds() {
    return (get_system_time() / g_hardware_frequency) * 1000000000ULL;
}

uint64_t kernel_timer::get_system_time_in_microseconds() {
    return (get_system_time() / g_hardware_frequency) * 1000000ULL;
}

uint64_t kernel_timer::get_system_time_in_milliseconds() {
    return (get_system_time() / g_hardware_frequency) * 1000ULL;
}

uint64_t kernel_timer::get_system_time_in_seconds() {
    return get_system_time() / g_hardware_frequency;
}

void sleep(uint32_t seconds) {
    auto& timer = acpi::hpet::get();
    uint64_t start = timer.read_counter();

    while (timer.read_counter() < start + (seconds * g_hardware_frequency)) {
        asm volatile ("pause");
    }
}

void msleep(uint32_t milliseconds) {
    auto& timer = acpi::hpet::get();
    uint64_t start = timer.read_counter();

    while (
        timer.read_counter() <
        start + (milliseconds * (g_hardware_frequency / 1000ULL))
    ) {
        asm volatile ("pause");
    }
}

void usleep(uint32_t microseconds) {
    auto& timer = acpi::hpet::get();
    uint64_t start = timer.read_counter();

    while (
        timer.read_counter() <
        start + (microseconds * (g_hardware_frequency / 1000000ULL))
    ) {
        asm volatile ("pause");
    }
}

void nanosleep(uint32_t nanoseconds) {
    auto& timer = acpi::hpet::get();
    uint64_t start = timer.read_counter();

    while (
        timer.read_counter() <
        start + (nanoseconds * (g_hardware_frequency / 1000000000ULL))
    ) {
        asm volatile ("pause");
    }
}
