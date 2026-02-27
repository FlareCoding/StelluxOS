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
#include "dynpriv/dynpriv.h"
#include "debug/debug.h"
#include "sched/task.h"
#include "fs/fs.h"
#include "exec/elf.h"

#ifdef STLX_UNIT_TESTS_ENABLED
#include "runner.h"
#endif

void log_cpu_id_entry(void*) {
    RUN_ELEVATED({
        sched::sleep_ms(sched::current()->tid * 100);
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

    if (rc::reaper::init() != rc::reaper::OK) {
        log::fatal("rc::reaper::init failed");
    }

    if (fs::init() != fs::OK) {
        log::fatal("fs::init failed");
    }

    if (clock::init() != clock::OK) {
        log::fatal("clock::init failed");
    }

    if (timer::init(100) != timer::OK) {
        log::fatal("timer::init failed");
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

    // Read a file from the initrd to verify extraction
    fs::file* f = fs::open("/initrd/hello.txt", fs::O_RDONLY);
    if (f) {
        char buf[128] = {};
        ssize_t n = fs::read(f, buf, sizeof(buf) - 1);
        if (n > 0) {
            log::info("initrd read: %s", buf);
        }
        fs::close(f);
    }

    exec::loaded_image loaded;
    int32_t load_result = exec::load_elf("/initrd/bin/init", &loaded);
    if (load_result == exec::OK) {
        log::info("ELF loaded: entry=0x%lx pt_root=0x%lx", loaded.entry_point, loaded.pt_root);
        sched::task* user_task = sched::create_user_task(loaded, "init");
        if (user_task) {
            log::info("User task created: tid=%u", user_task->tid);
        } else {
            log::error("Failed to create user task");
            exec::unload_elf(&loaded);
        }
    } else {
        log::error("ELF load of /initrd/bin/init failed: %d", load_result);
    }

    for (uint32_t i = 0; i < smp::cpu_count() * 4; i++) {
        sched::task* t = sched::create_kernel_task(log_cpu_id_entry, nullptr, "log_cpu_id_entry");
        sched::enqueue(t);
    }

    while (true) {
        cpu::halt();
    }
}
