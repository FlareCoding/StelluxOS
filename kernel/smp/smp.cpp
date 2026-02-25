#include "smp/smp.h"
#include "arch/arch_smp.h"
#include "common/logging.h"

static smp::cpu_info g_cpus[MAX_CPUS];
static uint32_t g_cpu_count = 0;
static bool g_smp_initialized = false;

namespace smp {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    if (g_smp_initialized) {
        return OK;
    }

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

    log::info("smp: %u CPUs discovered", g_cpu_count);

    if (ap_count == 0) {
        g_smp_initialized = true;
        return OK;
    }

    int32_t rc = arch::smp_prepare();
    if (rc != OK) {
        log::error("smp: prepare failed (%d)", rc);
        g_smp_initialized = true;
        return rc;
    }

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].is_bsp) continue;

        __atomic_store_n(&g_cpus[i].state, CPU_BOOTING, __ATOMIC_RELEASE);

        rc = arch::smp_boot_cpu(g_cpus[i]);
        if (rc == OK) {
            log::info("smp: CPU %u online (hw_id 0x%lx)",
                      g_cpus[i].logical_id, g_cpus[i].hw_id);
        } else {
            __atomic_store_n(&g_cpus[i].state, CPU_OFFLINE, __ATOMIC_RELEASE);
            log::warn("smp: CPU %u failed to start (hw_id 0x%lx)",
                      g_cpus[i].logical_id, g_cpus[i].hw_id);
        }
    }

    log::info("smp: %u/%u CPUs online", online_count(), g_cpu_count);
    g_smp_initialized = true;

    return OK;
}

uint32_t cpu_count() {
    return g_cpu_count;
}

uint32_t online_count() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (__atomic_load_n(&g_cpus[i].state, __ATOMIC_ACQUIRE) == CPU_ONLINE) {
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
