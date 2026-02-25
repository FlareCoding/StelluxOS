#include "smp/smp.h"
#include "arch/arch_smp.h"
#include "common/logging.h"

static smp::cpu_info g_cpus[MAX_CPUS];
static uint32_t g_cpu_count = 0;

namespace smp {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    g_cpu_count = arch::smp_enumerate(g_cpus, MAX_CPUS);

    if (g_cpu_count == 0) {
        log::error("smp: no CPUs found in MADT");
        return ERR_NO_CPUS;
    }

    uint32_t ap_count = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].is_bsp) {
            g_cpus[i].state = CPU_ONLINE;
        } else {
            g_cpus[i].state = CPU_OFFLINE;
            ap_count++;
        }
    }

    if (ap_count == 0) {
        log::info("smp: uniprocessor system (1 CPU discovered)");
    } else {
        log::info("smp: %u CPUs discovered", g_cpu_count);
    }

    return OK;
}

uint32_t cpu_count() {
    return g_cpu_count;
}

uint32_t online_count() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].state == CPU_ONLINE) {
            count++;
        }
    }
    return count;
}

cpu_info* get_cpu_info(uint32_t logical_id) {
    if (logical_id >= g_cpu_count) {
        return nullptr;
    }
    return &g_cpus[logical_id];
}

} // namespace smp
