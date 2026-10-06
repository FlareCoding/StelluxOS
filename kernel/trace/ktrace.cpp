#include "trace/ktrace.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "mm/kva.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "mm/vmm.h"
#include "percpu/percpu.h"
#include "smp/smp.h"
#include "sched/sched.h"

namespace ktrace {

constexpr size_t PERCPU_RECORD_BUFFER_RECORDS = 65536;
constexpr size_t PERCPU_RECORD_BUFFER_SIZE    = sizeof(trace_record) * PERCPU_RECORD_BUFFER_RECORDS; // 4 MB
constexpr size_t PERCPU_RECORD_BUFFER_PAGES   = PERCPU_RECORD_BUFFER_SIZE / paging::PAGE_SIZE_4KB; // 1024 pages

static_assert((PERCPU_RECORD_BUFFER_RECORDS & (PERCPU_RECORD_BUFFER_RECORDS - 1)) == 0,
              "record count must be a power of two");

constexpr uint64_t RING_CLOSED = 1ull << 63;

constexpr uint32_t SESSION_STOPPED   = 0;
constexpr uint32_t SESSION_RECORDING = 1;
constexpr uint32_t SESSION_STOPPING  = 2;

struct trace_ring {
    trace_record*          records;
    sync::atomic<uint64_t> head;
    sync::atomic<uint64_t> committed;
};

static DEFINE_PER_CPU_CACHELINE_ALIGNED(trace_ring, ktrace_percpu_ring);

static sync::spinlock g_session_lock = sync::SPINLOCK_INIT;
static sync::atomic<uint32_t> g_session_state;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
#if !defined(KTRACE_ENABLED) || KTRACE_ENABLED == 0
    return OK;
#endif

    uintptr_t virt_addr, _pa;
    int32_t rc = vmm::alloc_contiguous(
        PERCPU_RECORD_BUFFER_PAGES, pmm::ZONE_ANY,
        paging::PAGE_USER_RW | paging::PAGE_NORMAL,
        vmm::ALLOC_ALLOW_2MB | vmm::ALLOC_ZERO,
        kva::tag::generic,
        virt_addr, _pa
    );

    if (rc != vmm::OK) {
        return ERR_NO_MEMORY;
    }

    this_cpu(ktrace_percpu_ring).records = reinterpret_cast<trace_record*>(virt_addr);
    return OK;
}

int32_t start() {
    sync::lock_guard guard(g_session_lock);

    if (g_session_state.load_acquire() != SESSION_STOPPED) {
        return ERR_BUSY;
    }

    uint32_t cpu_count = smp::cpu_count();

    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        trace_ring& ring = per_cpu_on(ktrace_percpu_ring, cpu);

        // `committed` is zeroed before the head reopens, so
        // a writer that reserves right away keeps its commit.
        ring.committed.store_relaxed(0);
        ring.head.store_release(0);
    }

    g_session_state.store_release(SESSION_RECORDING);
    return OK;
}

// Closes a ring to new writers and waits until the slots already reserved are written
static void close_ring(trace_ring& ring) {
    uint64_t reserved = ring.head.fetch_or_relaxed(RING_CLOSED);

    while (ring.committed.load_acquire() != reserved) {
        sched::yield();
    }
}

int32_t stop() {
    {
        sync::lock_guard guard(g_session_lock);

        if (g_session_state.load_relaxed() != SESSION_RECORDING) {
            return ERR_NOT_RECORDING;
        }

        g_session_state.store_relaxed(SESSION_STOPPING);
    }

    uint32_t cpu_count = smp::cpu_count();

    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        close_ring(per_cpu_on(ktrace_percpu_ring, cpu));
    }

    g_session_state.store_release(SESSION_STOPPED);
    return OK;
}

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
void record_event(const trace_record& rec) {
    if (g_session_state.load_acquire() != SESSION_RECORDING) {
        return;
    }

    uint64_t timestamp = clock::now_ns();
    trace_ring& ring = this_cpu(ktrace_percpu_ring);

    if (!ring.records) {
        return;
    }

    // Atomically reserve a slot in the ring buffer
    uint64_t slot = ring.head.fetch_add_acquire(1);
    if (slot & RING_CLOSED) {
        return;
    }

    // Insert the event record
    trace_record& entry = ring.records[slot % PERCPU_RECORD_BUFFER_RECORDS];
    entry = rec;
    entry.hdr.timestamp = timestamp;

    // Mark the record as committed and make it visible to the other CPUs
    ring.committed.fetch_add_release(1);
}
#endif
} // namespace ktrace
