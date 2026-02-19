#include "boot_services.h"
#include "io/serial.h"
#include "common/logging.h"
#include "hw/cpu.h"
#include "arch/arch_init.h"
#include "mm/mm.h"
#if UNIT_TEST
#include "test/framework/test_runner.h"
#endif

/**
 * @brief Kernel entry point called by bootloader.
 * @note Privilege: **required**
 */
extern "C" __PRIVILEGED_CODE void stlx_init() {
#if UNIT_TEST
    bool tests_ok = true;
#endif

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

#if UNIT_TEST
    if (test::run_phase(test::phase::early) != test::OK) {
        tests_ok = false;
    }
#endif

    if (mm::init() != mm::OK) {
        log::fatal("mm::init failed");
    }

#if UNIT_TEST
    if (tests_ok && test::run_phase(test::phase::post_mm) != test::OK) {
        tests_ok = false;
    }

    test::print_summary();
    if (tests_ok && test::all_passed()) {
        log::info("[TEST_RESULT] PASS");
    } else {
        log::error("[TEST_RESULT] FAIL");
    }

    while (true) {
        cpu::halt();
    }
#endif

    log::debug("Initialization complete! Halting...");
    while (true) {
        cpu::halt();
    }
}
