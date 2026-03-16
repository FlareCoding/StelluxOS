#include "boot_services.h"
#include "io/serial.h"
#include "common/logging.h"
#include "hw/cpu.h"
#include "arch/arch_init.h"
#include "mm/mm.h"
#include "acpi/acpi.h"
#include "irq/irq.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "sched/sched.h"
#include "rc/reaper.h"
#include "smp/smp.h"
#include "debug/debug.h"
#include "sched/task.h"
#include "fs/fs.h"
#include "exec/elf.h"
#include "syscall/syscall_table.h"
#include "terminal/terminal.h"
#include "hw/rtc.h"
#include "pci/pci.h"

#ifdef STLX_UNIT_TESTS_ENABLED
#include "runner.h"
#endif

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

    if (pci::init() != pci::OK) {
        log::warn("pci::init failed, PCI devices unavailable");
    }

    if (rtc::init() != rtc::OK) {
        log::warn("rtc::init failed, wall-clock time unavailable");
    }

    if (irq::init() != irq::OK) {
        log::fatal("irq::init failed");
    }

    if (sched::init() != sched::OK) {
        log::fatal("sched::init failed");
    }

    if (rc::reaper::init() != rc::reaper::OK) {
        log::fatal("rc::reaper::init failed");
    }

    if (fs::init() != fs::OK) {
        log::fatal("fs::init failed");
    }

    if (terminal::init() != terminal::OK) {
        log::warn("terminal::init failed");
    }

    if (clock::init() != clock::OK) {
        log::fatal("clock::init failed");
    }

    if (timer::init(100) != timer::OK) {
        log::fatal("timer::init failed");
    }

    syscall::init_syscall_table();

    if (smp::init() != smp::OK) {
        log::warn("smp::init failed, continuing with single CPU");
    }

#ifdef STLX_UNIT_TESTS_ENABLED
    stlx_test::run_all();
    while (true) {
        cpu::halt();
    }
#endif

    exec::loaded_image loaded;
    int32_t load_result = exec::load_elf("/initrd/bin/init", &loaded);
    if (load_result == exec::OK) {
        sched::task* user_task = sched::create_user_task(&loaded, "init");
        if (user_task) {
            sched::enqueue(user_task);
        } else {
            log::error("Failed to create user task");
            exec::unload_elf(&loaded);
        }
    } else {
        log::error("ELF load of /initrd/bin/init failed: %d", load_result);
    }

    while (true) {
        cpu::halt();
    }
}
