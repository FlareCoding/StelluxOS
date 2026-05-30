#include "trace/trace.h"
#include "trace/trace_ts.h"
#include "trace/trace_categories.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "percpu/percpu.h"
#include "mm/heap.h"
#include "clock/clock.h"
#include "common/string.h"

namespace trace {

struct percpu_trace_state {
    trace_buffer*   ring_buffer;
};

static DEFINE_PER_CPU_CACHELINE_ALIGNED(percpu_trace_state, cpu_trace_state);

// Registry of every CPU's ring buffer, indexed by logical CPU id
static trace_buffer* g_buffers[MAX_CPUS] = {};

static category g_enabled_categories = category::all;
static category g_saved_categories   = category::none; // mask saved across a dump
static uint32_t g_dump_busy          = 0;              // 0 = idle, 1 = dump active

static bool category_enabled(category cat) {
    category enabled = __atomic_load_n(&g_enabled_categories, __ATOMIC_RELAXED);
    return (enabled & cat) != category::none;
}

/*
 *  ktrace dump wire format  (built by begin_dump, streamed by dump_read)
 *
 *  +================================================================+
 *  |  HEADER  (materialized in hdr_buf)                             |
 *  |    dump_header: magic "STLXTRC\0", version, arch, freq_hz,     |
 *  |                 cpu_count, total_size                          |
 *  |    count table: cpu_count x { cpu_id u32, record_count u32 }   |
 *  +================================================================+
 *  |  RECORDS  (streamed from the frozen ring buffers, 48B each)    |
 *  |    record: ts, name(id), arg0, arg1, tid, pid, cat, ph, fl     |
 *  |    name = interned id, never a kptr                            |
 *  |    arg0/arg1 = duration / next_tid / waker / prev_state        |
 *  +================================================================+
 *  |  STRINGS  (materialized in str_buf)                            |
 *  |    name_count u32, then name_count x { len u16, bytes[len] }   |
 *  |    a record id indexes this table  ->  resolves to a string    |
 *  +================================================================+
 *
 *  total_size = header + every record + string table (the fstat size)
 */
constexpr uint32_t DUMP_VERSION = 2;
constexpr uint32_t MAX_NAMES    = 1024;
constexpr uint32_t MAX_NAME_LEN = 64;

struct dump_header {
    char     magic[8];
    uint32_t version;
    uint32_t arch;
    uint64_t freq_hz;
    uint32_t cpu_count;
    uint32_t _pad;
    uint64_t total_size;
} __attribute__((packed));

struct cpu_range {
    uint32_t cpu_id;
    uint64_t start;
    uint32_t count;
    uint64_t stream_off;
};

struct dump_plan {
    dump_header header;
    cpu_range   cpus[MAX_CPUS];
    uint32_t    cpu_count;
    const char* names[MAX_NAMES];
    uint32_t    name_count;
    uint8_t     hdr_buf[sizeof(dump_header) + MAX_CPUS * 8];
    uint32_t    hdr_size;
    uint8_t     str_buf[4 + MAX_NAMES * (2 + MAX_NAME_LEN)];
    uint32_t    str_size;
    uint64_t    str_off;
    uint64_t    total_size;
};

static dump_plan g_plan;

static uint32_t intern_name(const char* name) {
    for (uint32_t i = 0; i < g_plan.name_count; ++i) {
        if (g_plan.names[i] == name) return i;
    }
    if (g_plan.name_count >= MAX_NAMES) return 0;
    g_plan.names[g_plan.name_count] = name;
    return g_plan.name_count++;
}

static uint32_t find_name_id(const char* name) {
    for (uint32_t i = 0; i < g_plan.name_count; ++i) {
        if (g_plan.names[i] == name) return i;
    }
    return 0;
}

static void build_plan() {
    g_plan.cpu_count = 0;
    g_plan.name_count = 0;

    for (uint32_t cpu = 0; cpu < MAX_CPUS; ++cpu) {
        trace_buffer* b = g_buffers[cpu];
        if (!b) continue;
        uint64_t head = b->head();
        uint64_t start = head < trace_buffer::CAPACITY ? 0 : head - trace_buffer::CAPACITY;
        uint32_t count = static_cast<uint32_t>(head - start);
        if (count == 0) continue;

        cpu_range& cr = g_plan.cpus[g_plan.cpu_count++];
        cr.cpu_id = cpu;
        cr.start  = start;
        cr.count  = count;
        for (uint64_t i = start; i < head; ++i) {
            intern_name(b->slot(i)->name);
        }
    }

    // str_buf: [u32 name_count][ (u16 len, bytes)* ]
    string::memcpy(g_plan.str_buf, &g_plan.name_count, 4);
    g_plan.str_size = 4;
    for (uint32_t i = 0; i < g_plan.name_count; ++i) {
        uint16_t len = static_cast<uint16_t>(string::strnlen(g_plan.names[i], MAX_NAME_LEN));
        string::memcpy(&g_plan.str_buf[g_plan.str_size], &len, 2);
        g_plan.str_size += 2;
        string::memcpy(&g_plan.str_buf[g_plan.str_size], g_plan.names[i], len);
        g_plan.str_size += len;
    }

    g_plan.header = {};
    string::memcpy(g_plan.header.magic, "STLXTRC", 8);
    g_plan.header.version = DUMP_VERSION;
    g_plan.header.arch    = ARCH_ID;
    g_plan.header.freq_hz = clock::freq_hz();
    g_plan.header.cpu_count = g_plan.cpu_count;

    g_plan.hdr_size = sizeof(dump_header) + g_plan.cpu_count * 8;
    uint64_t off = g_plan.hdr_size;
    for (uint32_t i = 0; i < g_plan.cpu_count; ++i) {
        g_plan.cpus[i].stream_off = off;
        off += static_cast<uint64_t>(g_plan.cpus[i].count) * sizeof(record);
    }
    g_plan.str_off = off;
    g_plan.total_size = off + g_plan.str_size;
    g_plan.header.total_size = g_plan.total_size;

    string::memcpy(g_plan.hdr_buf, &g_plan.header, sizeof(dump_header));
    uint32_t p = sizeof(dump_header);
    for (uint32_t i = 0; i < g_plan.cpu_count; ++i) {
        string::memcpy(&g_plan.hdr_buf[p], &g_plan.cpus[i].cpu_id, 4);
        p += 4;
        string::memcpy(&g_plan.hdr_buf[p], &g_plan.cpus[i].count, 4);
        p += 4;
    }
}

static const cpu_range* locate_cpu(uint64_t pos) {
    for (uint32_t i = 0; i < g_plan.cpu_count; ++i) {
        const cpu_range& cr = g_plan.cpus[i];
        if (pos >= cr.stream_off &&
            pos < cr.stream_off + static_cast<uint64_t>(cr.count) * sizeof(record)) {
            return &cr;
        }
    }
    return nullptr;
}

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

    build_plan();
    return OK;
}

void end_dump() {
    __atomic_store_n(&g_enabled_categories, g_saved_categories, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dump_busy, 0, __ATOMIC_RELEASE);
}

uint64_t dump_size() {
    return __atomic_load_n(&g_dump_busy, __ATOMIC_ACQUIRE) ? g_plan.total_size : 0;
}

size_t dump_read(uint64_t off, void* dst, size_t count) {
    if (!__atomic_load_n(&g_dump_busy, __ATOMIC_ACQUIRE)) return 0;
    if (off >= g_plan.total_size) return 0;
    if (off + count > g_plan.total_size) count = g_plan.total_size - off;

    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < count) {
        uint64_t pos = off + done;
        size_t rem = count - done;
        size_t n;
        if (pos < g_plan.hdr_size) {
            uint64_t avail = g_plan.hdr_size - pos;
            n = rem < avail ? rem : avail;
            string::memcpy(out + done, &g_plan.hdr_buf[pos], n);
        } else if (pos >= g_plan.str_off) {
            uint64_t avail = g_plan.str_size - (pos - g_plan.str_off);
            n = rem < avail ? rem : avail;
            string::memcpy(out + done, &g_plan.str_buf[pos - g_plan.str_off], n);
        } else {
            const cpu_range* cr = locate_cpu(pos);
            trace_buffer* b = g_buffers[cr->cpu_id];
            uint64_t rel = pos - cr->stream_off;
            uint32_t intra = static_cast<uint32_t>(rel % sizeof(record));
            const record* src = b->slot(cr->start + rel / sizeof(record));
            uint8_t wire[sizeof(record)];
            string::memcpy(wire, src, sizeof(record));
            uint64_t id = find_name_id(src->name);
            string::memcpy(&wire[__builtin_offsetof(record, name)], &id, sizeof(id));
            uint64_t avail = sizeof(record) - intra;
            n = rem < avail ? rem : avail;
            string::memcpy(out + done, &wire[intra], n);
        }
        done += n;
    }
    return done;
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

void emit_event(category cat, const char* name, phase ph,
                uint32_t tid, uint32_t pid,
                uint64_t arg0, uint64_t arg1, flags fl) {
    record rec {};
    rec.ts = timestamp();
    rec.name = name;
    rec.arg0 = arg0;
    rec.arg1 = arg1;
    rec.tid = tid;
    rec.pid = pid;
    rec.category = cat;
    rec.ph = ph;
    rec.fl = fl;
    emit_record(rec);
}

scope::scope(uint16_t category, const char* name) : m_category(category), m_name(name) {
    m_start_ts = timestamp();
}

scope::~scope() {
    sched::task* current_task = sched::current();

    record rec {};
    rec.ts = m_start_ts;
    rec.name = m_name;
    rec.arg0 = timestamp() - m_start_ts;
    rec.tid = current_task ? current_task->tid : static_cast<uint32_t>(-1);
    rec.pid = current_task ? sched::process_id(current_task) : 0;
    rec.category = m_category;
    rec.ph = phase::complete;
    rec.fl = flags::none;

    emit_record(rec);
}

} // namespace trace
