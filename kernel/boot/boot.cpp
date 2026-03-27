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
#include "dynpriv/dynpriv.h"
#include "msi/msi.h"
#include "drivers/pci_driver.h"
#include "drivers/platform_driver.h"
#include "drivers/graphics/gfxfb.h"
#include "drivers/input/input.h"
#include "net/net.h"

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
    } else {
        // Many x86 machines (especially desktops) lack a built-in COM1 port,
        // making serial debugging an issue. A PCI serial adapter plugged
        // into a motherboard slot provides one. If we find a PCI serial
        // controller, redirect all serial output to it.
        pci::device* serial_pci = pci::find_by_class(0x07, 0x00);
        if (serial_pci) {
            const auto& bar = serial_pci->get_bar(0);
            if (bar.type == pci::BAR_IO && bar.phys != 0 && bar.phys != 0x3F8) {
                RUN_ELEVATED(serial_pci->enable());
                serial::set_port(static_cast<uint16_t>(bar.phys));
                log::info("serial: redirected to PCI adapter at %02x:%02x.%x (port 0x%x)",
                          serial_pci->bus(), serial_pci->slot(), serial_pci->func(),
                          static_cast<uint32_t>(bar.phys));
            }
        }
    }

    if (rtc::init() != rtc::OK) {
        log::warn("rtc::init failed, wall-clock time unavailable");
    }

    if (irq::init() != irq::OK) {
        log::fatal("irq::init failed");
    }

    if (msi::init() != msi::OK) {
        log::warn("msi::init failed, MSI unavailable");
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

    if (gfxfb::init() != gfxfb::OK) {
        log::warn("gfxfb::init failed, framebuffer unavailable");
    }

    if (input::init() != input::OK) {
        log::warn("input::init failed, input devices unavailable");
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

    if (net::init() != net::OK) {
        log::warn("net::init failed, networking unavailable");
    }

    if (drivers::init() != drivers::OK) {
        log::warn("drivers::init failed");
    }

    if (drivers::platform_init() != drivers::OK) {
        log::warn("drivers::platform_init failed");
    }

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
