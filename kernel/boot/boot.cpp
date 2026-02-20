#include "boot_services.h"
#include "io/serial.h"
#include "common/logging.h"
#include "hw/cpu.h"
#include "arch/arch_init.h"
#include "mm/mm.h"

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

#ifdef STLX_UNIT_TESTS_ENABLED
    stlx_test::run_all();
    while (true) {
        cpu::halt();
    }
#endif

    log::debug("Initialization complete! Halting...");
    while (true) {
        cpu::halt();
    }
}
