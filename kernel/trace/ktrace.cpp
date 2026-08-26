#include "trace/ktrace.h"
#include "common/logging.h"
#include "sync/atomic.h"
#include "mm/kva.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "mm/vmm.h"
#include "percpu/percpu.h"

namespace ktrace {

constexpr size_t PERCPU_RECORD_BUFFER_RECORDS = 65536;
constexpr size_t PERCPU_RECORD_BUFFER_SIZE    = sizeof(trace_record) * PERCPU_RECORD_BUFFER_RECORDS; // 4 MB
constexpr size_t PERCPU_RECORD_BUFFER_PAGES   = PERCPU_RECORD_BUFFER_SIZE / paging::PAGE_SIZE_4KB; // 1024 pages

static_assert((PERCPU_RECORD_BUFFER_RECORDS & (PERCPU_RECORD_BUFFER_RECORDS - 1)) == 0,
              "record count must be a power of two");

DEFINE_PER_CPU(trace_record*, ktrace_percpu_record_buffer);
DEFINE_PER_CPU_CACHELINE_ALIGNED(uint64_t, ktrace_percpu_head_index);

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

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
void record_event(const trace_record& rec) {
    auto& buffer = this_cpu(ktrace_percpu_record_buffer);
    auto& head_idx = this_cpu(ktrace_percpu_head_index);

    if (!buffer) {
        // Possible in early kernel initialization window
        return;
    }

    // Atomically reserve a slot in the ring buffer 
    uint64_t slot = sync::atomic_ref<uint64_t>{head_idx}.fetch_add_relaxed(1);

    // Insert the event record
    buffer[slot % PERCPU_RECORD_BUFFER_RECORDS] = rec;
}
#endif
} // namespace ktrace
