#include "smp/ipi.h"
#include "smp/smp.h"
#include "arch/arch_smp.h"
#include "percpu/percpu.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"

namespace smp {
namespace ipi {

// Each CPU's pending word sits on its own cache line so senders targeting
// different CPUs never contend with each other.
struct alignas(64) pending_word {
    uint32_t bits;
};

__PRIVILEGED_DATA static pending_word g_pending[MAX_CPUS] = {};
__PRIVILEGED_DATA static handler g_handlers[MAX_MESSAGES] = {};
__PRIVILEGED_DATA static uint32_t g_registered = 0;
__PRIVILEGED_DATA static sync::spinlock g_registry_lock = sync::SPINLOCK_INIT;

// A valid message is a single bit at or below the highest registered slot
__PRIVILEGED_CODE static bool is_registered(message m) {
    if (m.bit == 0 || (m.bit & (m.bit - 1)) != 0) {
        return false;
    }

    uint32_t index = static_cast<uint32_t>(__builtin_ctz(m.bit));
    return index < sync::atomic_ref<uint32_t>{g_registered}.load_acquire();
}

// The fence publishes the pending word before the interrupt can be seen.
// Addressability of a CPU never changes, so undoing the mark after a failed
// raise cannot drop a message that another sender could have delivered.
__PRIVILEGED_CODE static int32_t deliver(cpu_info& target, message m) {
    sync::atomic_ref<uint32_t> pending{g_pending[target.logical_id].bits};
    pending.fetch_or_release(m.bit);
    sync::atomic_fence_release();

    int32_t rc = arch::smp_raise_ipi(target);
    if (rc != OK) {
        pending.fetch_and_relaxed(~m.bit);
    }

    return rc;
}

__PRIVILEGED_CODE int32_t init() {
    return arch::smp_ipi_init();
}

__PRIVILEGED_CODE int32_t init_ap() {
    return arch::smp_ipi_init_ap();
}

__PRIVILEGED_CODE int32_t register_message(handler fn, message* out) {
    if (!fn || !out) {
        return ERR_INVALID;
    }

    sync::irq_lock_guard guard(g_registry_lock);

    if (g_registered == MAX_MESSAGES) {
        return ERR_FULL;
    }

    g_handlers[g_registered] = fn;
    out->bit = 1u << g_registered;
    sync::atomic_ref<uint32_t>{g_registered}.store_release(g_registered + 1);

    return OK;
}

__PRIVILEGED_CODE int32_t send(uint32_t cpu, message m) {
    if (!is_registered(m)) {
        return ERR_INVALID;
    }

    cpu_info* target = get_cpu_info(cpu);
    if (!target || target->state.load_acquire() != CPU_ONLINE) {
        return ERR_OFFLINE;
    }

    return deliver(*target, m);
}

__PRIVILEGED_CODE uint32_t send_all_but_self(message m) {
    if (!is_registered(m)) {
        return 0;
    }

    uint32_t self = percpu::current_cpu_id();
    uint32_t sent = 0;
    for (uint32_t cpu = 0; cpu < cpu_count(); cpu++) {
        cpu_info* target = get_cpu_info(cpu);
        if (cpu == self || target->state.load_acquire() != CPU_ONLINE) {
            continue;
        }

        if (deliver(*target, m) == OK) {
            sent++;
        }
    }

    return sent;
}

__PRIVILEGED_CODE void dispatch() {
    uint32_t cpu = percpu::current_cpu_id();
    uint32_t bits = sync::atomic_ref<uint32_t>{g_pending[cpu].bits}.exchange_acquire(0);

    while (bits != 0) {
        uint32_t index = static_cast<uint32_t>(__builtin_ctz(bits));
        bits &= bits - 1;
        g_handlers[index]();
    }
}

} // namespace ipi
} // namespace smp
