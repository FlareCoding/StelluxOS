#include "trace/trace.h"
#include "trace/trace_ts.h"
#include "trace/trace_categories.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "percpu/percpu.h"
#include "mm/heap.h"

namespace trace {

namespace {
struct percpu_trace_state {
    trace_buffer*   ring_buffer;
};

DEFINE_PER_CPU_CACHELINE_ALIGNED(percpu_trace_state, cpu_trace_state);

// Registry of every CPU's ring buffer, indexed by logical CPU id
trace_buffer* g_buffers[MAX_CPUS] = {};

category g_enabled_categories = category::all;
category g_saved_categories   = category::none; // mask saved across a dump
uint32_t g_dump_busy          = 0;              // 0 = idle, 1 = dump active


bool category_enabled(category cat) {
    category enabled = __atomic_load_n(&g_enabled_categories, __ATOMIC_RELAXED);
    return (enabled & cat) != category::none;
}
} // namespace (anonymous)

int32_t begin_dump() {
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_dump_busy, &expected, 1u,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return ERR_BUSY; // a dump is already in progress
    }

    // Freeze capture so the buffers are stable while they are read
    g_saved_categories = __atomic_load_n(&g_enabled_categories, __ATOMIC_RELAXED);
    __atomic_store_n(&g_enabled_categories, category::none, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return OK;
}

void end_dump() {
    __atomic_store_n(&g_enabled_categories, g_saved_categories, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dump_busy, 0, __ATOMIC_RELEASE);
}

void reset_buffers() {
    for (uint32_t cpu = 0; cpu < MAX_CPUS; ++cpu) {
        if (g_buffers[cpu]) {
            g_buffers[cpu]->clear();
        }
    }
}

int32_t init() {
    auto& trace_state = this_cpu(cpu_trace_state);
    if (trace_state.ring_buffer) {
        return OK; // already initialized on this CPu
    }

    // Allocate the ring buffer for this cpu
    trace_state.ring_buffer = heap::ualloc_new<trace_buffer>();
    if (!trace_state.ring_buffer) {
        return ERR_NO_MEM;
    }

    g_buffers[percpu::current_cpu_id()] = trace_state.ring_buffer;
    return OK;
}

void set_enabled_categories(category mask) {
    __atomic_store_n(&g_enabled_categories, mask, __ATOMIC_RELAXED);
}
uint16_t enabled_categories() {
    return __atomic_load_n(&g_enabled_categories, __ATOMIC_RELAXED);
}

void emit_record(const record& rec) {
    if (!category_enabled(static_cast<category>(rec.category))) {
        return;
    }

    auto& trace_state = this_cpu(cpu_trace_state);
    if (trace_state.ring_buffer) {
        trace_state.ring_buffer->push(rec);
    }
}

scope::scope(uint16_t category, const char* name) : m_category(category), m_name(name) {
    m_start_ts = timestamp();
}

scope::~scope() {
    sched::task* current_task = sched::current();

    record rec {};
    rec.ts = m_start_ts;
    rec.name = m_name;
    rec.arg = timestamp() - m_start_ts;
    rec.tid = current_task ? current_task->tid : static_cast<uint32_t>(-1);
    rec.category = m_category;
    rec.ph = phase::complete;
    rec.fl = flags::none;

    emit_record(rec);
}

} // namespace trace
