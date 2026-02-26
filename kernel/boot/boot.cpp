#include "boot_services.h"
#include "io/serial.h"
#include "common/logging.h"
#include "hw/cpu.h"
#include "arch/arch_init.h"
#include "mm/mm.h"
#include "acpi/acpi.h"
#include "irq/irq.h"
#include "hwtimer/hwtimer.h"
#include "sched/sched.h"
#include "smp/smp.h"
#include "dynpriv/dynpriv.h"
#include "debug/debug.h"
#include "sched/task.h"

#ifdef STLX_UNIT_TESTS_ENABLED
#include "runner.h"
#endif

void log_cpu_id_entry(void*) {
    RUN_ELEVATED({
        log::info("Task %u is on core %u!", sched::current()->tid, percpu::current_cpu_id());
    });
    sched::exit(0);
}

/**
 * @brief Kernel entry point called by bootloader.
 * @note Privilege: **required**
 */
extern "C" __PRIVILEGED_CODE void stlx_init() {
    if (boot_services::init() != boot_services::OK) {
        cpu::halt();
    }

    if (serial::init() != serial::OK) {
        cpu::halt();
    }

    if (arch::early_init() != arch::OK) {
        log::fatal("arch::early_init failed");
    }

    log::info("Stellux 3.0 booting...");

    if (mm::init() != mm::OK) {
        log::fatal("mm::init failed");
    }

    if (serial::remap() != serial::OK) {
        log::fatal("serial::remap failed");
    }

    if (debug::init() != debug::OK) {
        log::fatal("debug::init failed");
    }

    if (acpi::init() != acpi::OK) {
        log::fatal("acpi::init failed");
    }

    if (irq::init() != irq::OK) {
        log::fatal("irq::init failed");
    }

    if (sched::init() != sched::OK) {
        log::fatal("sched::init failed");
    }

    if (hwtimer::init(100) != hwtimer::OK) {
        log::fatal("hwtimer::init failed");
    }

    if (smp::init() != smp::OK) {
        log::warn("smp::init failed, continuing with single CPU");
    }

#ifdef STLX_UNIT_TESTS_ENABLED
    stlx_test::run_all();
    while (true) {
        cpu::halt();
    }
#endif

    for (uint32_t i = 0; i < smp::cpu_count() * 4; i++) {
        sched::task* t = sched::create_kernel_task(log_cpu_id_entry, nullptr, "log_cpu_id_entry");
        sched::enqueue(t);
    }

    log::debug("Initialization complete! Halting...");
    while (true) {
        cpu::halt();
    }
}
