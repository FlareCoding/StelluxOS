#include "mm/paging.h"
#include "mm/paging_arch.h"
#include "mm/tlb_shootdown.h"
#include "smp/ipi.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "clock/clock.h"
#include "hw/cpu.h"
#include "sync/atomic.h"
#include "common/logging.h"

namespace paging {

// A CPU that has not acknowledged after this long is named in the log
constexpr uint64_t ACK_WARNING_NS = 1000000000ULL;

// The flush every other CPU must apply. One request is in flight at a time,
// so the fields are only rewritten after every CPU acknowledged the last one.
struct flush_request {
    virt_addr_t start;
    virt_addr_t end;
    bool        full;
};

// Each CPU records the generation of the last request it applied, on its own
// cache line so the initiator's polling never disturbs the other writers.
struct alignas(64) ack_word {
    uint64_t generation;
};

__PRIVILEGED_DATA static flush_request g_request = {};
__PRIVILEGED_DATA static uint64_t g_generation = 0;
__PRIVILEGED_DATA static ack_word g_acks[MAX_CPUS] = {};
__PRIVILEGED_DATA static smp::ipi::message g_message = {0};

// Held with interrupts disabled so the holder cannot be preempted while other
// CPUs wait on it, and waited for with interrupts enabled so a waiter still
// acknowledges the flush already in flight. A plain spinlock gives neither.
__PRIVILEGED_DATA static sync::atomic<uint32_t> g_initiator_busy{0};

// One instruction per page, no page table walk, so it is safe in the
// interrupt handler. A huge page is invalidated by any address inside it.
__PRIVILEGED_CODE static void apply_locally(const flush_request& request) {
    if (request.full) {
        flush_tlb_all_local();
        return;
    }

    for (virt_addr_t addr = request.start; addr < request.end; addr += PAGE_SIZE_4KB) {
        invlpg(addr);
    }
}

__PRIVILEGED_CODE static void on_flush_request() {
    uint64_t generation = sync::atomic_ref<uint64_t>{g_generation}.load_acquire();
    apply_locally(g_request);

    uint32_t cpu = percpu::current_cpu_id();
    sync::atomic_ref<uint64_t>{g_acks[cpu].generation}.store_release(generation);
}

// Interrupts every other online CPU and returns the set that was reached, so
// the caller waits for exactly the CPUs that received the request.
__PRIVILEGED_CODE static uint64_t request_from_others() {
    uint32_t self = percpu::current_cpu_id();
    uint64_t targets = 0;

    for (uint32_t cpu = 0; cpu < smp::cpu_count(); cpu++) {
        if (cpu == self) {
            continue;
        }

        if (smp::ipi::send(cpu, g_message) == smp::ipi::OK) {
            targets |= 1ULL << cpu;
        }
    }

    return targets;
}

__PRIVILEGED_CODE static uint64_t acquire_initiator() {
    while (true) {
        uint64_t flags = cpu::irq_save();
        uint32_t idle = 0;
        if (g_initiator_busy.cmpxchg_strong_acquire(idle, 1)) {
            return flags;
        }

        cpu::irq_restore(flags);
        cpu::relax();
    }
}

__PRIVILEGED_CODE static void release_initiator(uint64_t flags) {
    g_initiator_busy.store_release(0);
    cpu::irq_restore(flags);
}

__PRIVILEGED_CODE static void wait_for_acks(uint64_t targets, uint64_t generation) {
    uint64_t deadline = clock::now_ns() + ACK_WARNING_NS;

    for (uint32_t cpu = 0; cpu < smp::cpu_count(); cpu++) {
        if ((targets & (1ULL << cpu)) == 0) {
            continue;
        }

        sync::atomic_ref<uint64_t> ack{g_acks[cpu].generation};
        bool warned = false;
        while (ack.load_acquire() < generation) {
            cpu::relax();
            if (!warned && clock::now_ns() > deadline) {
                log::warn("paging: CPU %u has not acknowledged a TLB flush", cpu);
                warned = true;
            }
        }
    }
}

__PRIVILEGED_CODE static void flush_everywhere(const flush_request& request) {
    // Before the other CPUs run there is nobody to ask
    if (smp::online_count() <= 1) {
        apply_locally(request);
        return;
    }

    if (!cpu::irqs_enabled()) {
        log::fatal("paging: system-wide TLB flush requested with interrupts disabled");
    }

    uint64_t flags = acquire_initiator();

    g_request = request;
    uint64_t generation = g_generation + 1;
    sync::atomic_ref<uint64_t>{g_generation}.store_release(generation);

    uint64_t targets = request_from_others();
    apply_locally(request);
    wait_for_acks(targets, generation);

    release_initiator(flags);
}

__PRIVILEGED_CODE void flush_tlb_page(virt_addr_t virt) {
    flush_everywhere({virt, virt + PAGE_SIZE_4KB, false});
}

__PRIVILEGED_CODE void flush_tlb_range(virt_addr_t start, virt_addr_t end) {
    bool full = (end - start) / PAGE_SIZE_4KB > FULL_FLUSH_PAGE_THRESHOLD;
    flush_everywhere({start, end, full});
}

__PRIVILEGED_CODE void flush_tlb_all() {
    flush_everywhere({0, 0, true});
}

} // namespace paging

namespace x86 {

__PRIVILEGED_CODE int32_t init_tlb_shootdown() {
    int32_t rc = smp::ipi::register_message(paging::on_flush_request, &paging::g_message);
    return rc == smp::ipi::OK ? paging::OK : paging::ERR_NO_RESOURCE;
}

} // namespace x86
