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

namespace ktrace {

constexpr size_t PERCPU_RECORD_BUFFER_RECORDS = 65536;
constexpr size_t PERCPU_RECORD_BUFFER_SIZE    = sizeof(trace_record) * PERCPU_RECORD_BUFFER_RECORDS; // 4 MB
constexpr size_t PERCPU_RECORD_BUFFER_PAGES   = PERCPU_RECORD_BUFFER_SIZE / paging::PAGE_SIZE_4KB; // 1024 pages

static_assert((PERCPU_RECORD_BUFFER_RECORDS & (PERCPU_RECORD_BUFFER_RECORDS - 1)) == 0,
              "record count must be a power of two");

DEFINE_PER_CPU(trace_record*, ktrace_percpu_record_buffer);
DEFINE_PER_CPU_CACHELINE_ALIGNED(uint64_t, ktrace_percpu_head_index);

static sync::spinlock g_session_lock = sync::SPINLOCK_INIT;
static sync::atomic<bool> g_recording;

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

    this_cpu(ktrace_percpu_record_buffer) = reinterpret_cast<trace_record*>(virt_addr);
    return OK;
}

int32_t start() {
    sync::lock_guard guard(g_session_lock);

    if (g_recording.load_relaxed()) {
        return ERR_BUSY;
    }

    uint32_t cpu_count = smp::cpu_count();

    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        sync::atomic_ref<uint64_t>{per_cpu_on(ktrace_percpu_head_index, cpu)}.store_relaxed(0);
    }

    g_recording.store_release(true);
    return OK;
}

int32_t stop() {
    sync::lock_guard guard(g_session_lock);

    if (!g_recording.load_relaxed()) {
        return ERR_NOT_RECORDING;
    }

    g_recording.store_relaxed(false);
    return OK;
}

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
void record_event(const trace_record& rec) {
    if (!g_recording.load_acquire()) {
        return;
    }

    uint64_t timestamp = clock::now_ns();
    auto& buffer = this_cpu(ktrace_percpu_record_buffer);
    auto& head_idx = this_cpu(ktrace_percpu_head_index);

    if (!buffer) {
        return;
    }

    // Atomically reserve a slot in the ring buffer
    uint64_t slot = sync::atomic_ref<uint64_t>{head_idx}.fetch_add_relaxed(1);

    // Insert the event record
    trace_record& entry = buffer[slot % PERCPU_RECORD_BUFFER_RECORDS];
    entry = rec;
    entry.hdr.timestamp = timestamp;
}
#endif
} // namespace ktrace
