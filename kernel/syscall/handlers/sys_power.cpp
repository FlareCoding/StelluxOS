#include "syscall/handlers/sys_power.h"

#include "power/power.h"
#include "hw/cpu.h"
#include "common/logging.h"

// Linux reboot(2) magic values, required so a stray syscall with a
// garbage number cannot take the machine down by accident.
constexpr uint32_t REBOOT_MAGIC1  = 0xFEE1DEAD;
constexpr uint32_t REBOOT_MAGIC2  = 672274793;
constexpr uint32_t REBOOT_MAGIC2A = 85072278;
constexpr uint32_t REBOOT_MAGIC2B = 369367448;
constexpr uint32_t REBOOT_MAGIC2C = 537993216;

// Linux reboot(2) commands
constexpr uint32_t REBOOT_CMD_RESTART   = 0x01234567;
constexpr uint32_t REBOOT_CMD_HALT      = 0xCDEF0123;
constexpr uint32_t REBOOT_CMD_POWER_OFF = 0x4321FEDC;

DEFINE_SYSCALL3(reboot, magic1, magic2, cmd) {
    if (static_cast<uint32_t>(magic1) != REBOOT_MAGIC1) {
        return syscall::EINVAL;
    }

    uint32_t m2 = static_cast<uint32_t>(magic2);
    if (m2 != REBOOT_MAGIC2 && m2 != REBOOT_MAGIC2A &&
        m2 != REBOOT_MAGIC2B && m2 != REBOOT_MAGIC2C) {
        return syscall::EINVAL;
    }

    switch (static_cast<uint32_t>(cmd)) {
    case REBOOT_CMD_RESTART:
        log::info("power: restarting system");
        power::reboot();
        return syscall::EOPNOTSUPP;
    case REBOOT_CMD_POWER_OFF:
        log::info("power: powering off");
        power::shutdown();
        return syscall::EOPNOTSUPP;
    case REBOOT_CMD_HALT:
        log::info("power: system halted");
        cpu::irq_disable();
        while (true) {
            cpu::halt();
        }
    default:
        return syscall::EINVAL;
    }
}
