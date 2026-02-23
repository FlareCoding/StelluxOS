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
#include "dynpriv/dynpriv.h"

#ifdef STLX_UNIT_TESTS_ENABLED
#include "runner.h"
#endif

int FIB_N = 10;

int fibonacci(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    if (n == 2) return 1;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

void fib_task_main(void* arg) {
    int n = *((int*)arg);
    int result = fibonacci(n);

    if (n == 10) {
        int n2 = 20;
        RUN_ELEVATED({
            sched::task* fib_task2 = sched::create_kernel_task(fib_task_main, &n2, "fib_task2");
            sched::enqueue(fib_task2);
        });
    }
    
    log::info("fibonacci(%d) = %d", n, result);

    sched::exit();
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
    
#ifdef STLX_UNIT_TESTS_ENABLED
    stlx_test::run_all();
    while (true) {
        cpu::halt();
    }
#endif

    sched::task* fib_task = sched::create_kernel_task(fib_task_main, &FIB_N, "fib_task");
    sched::enqueue(fib_task);

    log::debug("Initialization complete! Halting...");
    while (true) {
        cpu::halt();
    }
}
