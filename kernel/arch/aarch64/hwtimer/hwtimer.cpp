#include "hwtimer/hwtimer.h"
#include "hwtimer/hwtimer_arch.h"
#include "irq/irq.h"
#include "hw/cpu.h"
#include "common/logging.h"

namespace hwtimer {

__PRIVILEGED_BSS static uint32_t g_interval;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void rearm() {
    write_cntv_tval(g_interval);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(uint32_t hz) {
    uint32_t freq = read_cntfrq();
    if (freq == 0) {
        log::error("hwtimer: CNTFRQ_EL0 is zero");
        return ERR;
    }

    g_interval = freq / hz;

    // Set first tick and enable timer (ENABLE=1, IMASK=0)
    write_cntv_tval(g_interval);
    write_cntv_ctl(1);

    // Unmask PPI 27 in GIC
    irq::unmask(TIMER_PPI);

    // Enable CPU interrupts so timer IRQs can be delivered
    cpu::irq_enable();

    log::info("hwtimer: Generic Timer freq=%u Hz, periodic at %u Hz (interval=%u)",
              freq, hz, g_interval);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void stop() {
    // Disable timer (ENABLE=0)
    write_cntv_ctl(0);

    // Mask PPI 27
    irq::mask(TIMER_PPI);
}

} // namespace hwtimer
