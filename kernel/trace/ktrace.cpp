#include "trace/ktrace.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "exec/elf_arch.h"
#include "fs/devfs/devfs.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "mm/heap.h"
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

constexpr uint32_t SESSION_IDLE      = 0;
constexpr uint32_t SESSION_RECORDING = 1;
constexpr uint32_t SESSION_STOPPING  = 2;
constexpr uint32_t SESSION_STOPPED   = 3;

constexpr uint64_t DRAIN_INTERVAL_NS    = 100000000ULL;
constexpr size_t   SMALLEST_CHUNK_BYTES = sizeof(chunk_header) + sizeof(trace_record);

namespace {

struct trace_ring {
    trace_record*          records;
    sync::atomic<uint64_t> head;      // Slots reserved, RING_CLOSED once stopped
    sync::atomic<uint64_t> committed; // Records completely written
    sync::atomic<uint64_t> drained;   // Records copied to the reader
    sync::atomic<uint64_t> lost;      // Records dropped while the ring was full
    uint64_t               lost_reported;
};

struct trace_reader {
    bool     session_started;
    bool     header_sent;
    bool     end_sent;
    bool     read_in_progress;
    bool     last_read_was_full;
    uint32_t drain_start_cpu;
    uint64_t last_drain_ns;
};

// /dev/ktrace/control, where each write is one command, `start` or `stop`
class control_node : public fs::node {
public:
    control_node() : fs::node(fs::node_type::char_device, nullptr, "control") {}

    ssize_t write(fs::file*, const void* buf, size_t count, uint32_t) override {
        const char* text = static_cast<const char*>(buf);

        int32_t rc;
        if (is_command(text, count, "start")) {
            rc = start(ALL_EVENTS);
        } else if (is_command(text, count, "stop")) {
            rc = stop();
        } else {
            return fs::ERR_INVAL;
        }

        if (rc == ERR_BUSY) {
            return fs::ERR_BUSY;
        }

        if (rc == ERR_NO_READER) {
            return fs::ERR_PIPE;
        }

        if (rc != OK) {
            return fs::ERR_INVAL;
        }

        return static_cast<ssize_t>(count);
    }

private:
    static bool is_command(const char* text, size_t length, const char* command) {
        size_t command_length = string::strlen(command);
        if (length == command_length + 1 && text[command_length] == '\n') {
            length = command_length;
        }

        return length == command_length && string::memcmp(text, command, command_length) == 0;
    }
};

class trace_node : public fs::node {
public:
    trace_node() : fs::node(fs::node_type::char_device, nullptr, "trace") {}

    int32_t open(fs::file* f, uint32_t flags) override;
    int32_t on_close(fs::file* f) override;
    ssize_t read(fs::file* f, void* buf, size_t count, uint32_t flags) override;
};

} // anonymous namespace

static DEFINE_PER_CPU_CACHELINE_ALIGNED(trace_ring, ktrace_percpu_ring);

static sync::spinlock g_session_lock = sync::SPINLOCK_INIT;
static sync::atomic<uint32_t> g_session_state;
static sync::atomic<uint64_t> g_session_start_ns;
static sync::atomic<uint64_t> g_session_stop_ns;
static sync::atomic<uint64_t> g_session_event_mask;
static sync::atomic<uint64_t> g_bytes_streamed;
static uint32_t g_trace_open_count;
static trace_reader g_reader;

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

static const char* session_state_name(uint32_t state) {
    switch (state) {
    case SESSION_IDLE:      return "idle";
    case SESSION_RECORDING: return "recording";
    case SESSION_STOPPING:  return "stopping";
    case SESSION_STOPPED:   return "stopped";
    default:                return "unknown";
    }
}

// `key value` lines, then one `cpu<N> <records> <lost>` line per CPU
static size_t generate_status(char* buf, size_t cap) {
    uint32_t state;
    uint64_t start_ns;
    uint64_t stop_ns;
    uint32_t trace_open;

    {
        sync::lock_guard guard(g_session_lock);

        state = g_session_state.load_acquire();
        start_ns = g_session_start_ns.load_relaxed();
        stop_ns = g_session_stop_ns.load_relaxed();
        trace_open = g_trace_open_count;
    }

    size_t pos = 0;
    pos = devfs::append_str(buf, cap, pos, "state ");
    pos = devfs::append_str(buf, cap, pos, session_state_name(state));
    pos = devfs::append_str(buf, cap, pos, "\nsession_start_ns ");
    pos = devfs::append_u64(buf, cap, pos, start_ns);

    pos = devfs::append_str(buf, cap, pos, "\nsession_stop_ns ");
    pos = devfs::append_u64(buf, cap, pos, stop_ns);
    pos = devfs::append_str(buf, cap, pos, "\ncapacity ");
    pos = devfs::append_u64(buf, cap, pos, PERCPU_RECORD_BUFFER_RECORDS);

    pos = devfs::append_str(buf, cap, pos, "\ntrace_open ");
    pos = devfs::append_u64(buf, cap, pos, trace_open);
    pos = devfs::append_str(buf, cap, pos, "\nbytes_streamed ");
    pos = devfs::append_u64(buf, cap, pos, g_bytes_streamed.load_relaxed());
    pos = devfs::append_str(buf, cap, pos, "\n");

    uint32_t cpu_count = smp::cpu_count();

    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        trace_ring& ring = per_cpu_on(ktrace_percpu_ring, cpu);

        pos = devfs::append_str(buf, cap, pos, "cpu");
        pos = devfs::append_u64(buf, cap, pos, cpu);
        pos = devfs::append_str(buf, cap, pos, " ");
        pos = devfs::append_u64(buf, cap, pos, ring.drained.load_relaxed());
        pos = devfs::append_str(buf, cap, pos, " ");
        pos = devfs::append_u64(buf, cap, pos, ring.lost.load_relaxed());
        pos = devfs::append_str(buf, cap, pos, "\n");
    }

    return pos;
}

static file_header build_file_header() {
    file_header header = {};
    string::memcpy(header.magic, FILE_MAGIC, sizeof(header.magic));

    header.version = FILE_VERSION;
    header.header_size = sizeof(file_header);
    header.arch = exec::ELF_EXPECTED_MACHINE;
    header.cpu_count = smp::cpu_count();
    header.record_size = sizeof(trace_record);
    header.boot_unix_ns = clock::boot_realtime_ns();
    header.session_start_ns = g_session_start_ns.load_relaxed();
    header.event_mask = g_session_event_mask.load_relaxed();

    return header;
}

// Only a moment with no write in progress shows where the written records end
static bool find_written_end(trace_ring& ring, uint64_t* end) {
    uint64_t reserved = ring.head.load_acquire() & ~RING_CLOSED;
    uint64_t written = ring.committed.load_acquire();
    if (written != reserved || (ring.head.load_acquire() & ~RING_CLOSED) != reserved) {
        return false;
    }

    *end = reserved;
    return true;
}

static size_t drain_ring_chunk(uint8_t* dst, size_t room, uint32_t cpu, trace_ring& ring) {
    uint64_t end = 0;
    if (!ring.records || room < sizeof(chunk_header) || !find_written_end(ring, &end)) {
        return 0;
    }

    uint64_t start = ring.drained.load_relaxed();
    uint64_t lost = ring.lost.load_relaxed();
    uint64_t fits = (room - sizeof(chunk_header)) / sizeof(trace_record);
    uint64_t count = end - start < fits ? end - start : fits;

    if (count == 0 && lost == ring.lost_reported) {
        return 0;
    }

    chunk_header chunk = {};
    chunk.kind = CHUNK_RECORDS;
    chunk.cpu_id = cpu;
    chunk.record_count = count;
    chunk.lost_records = lost - ring.lost_reported;

    uint64_t first = start % PERCPU_RECORD_BUFFER_RECORDS;
    uint64_t before_wrap = PERCPU_RECORD_BUFFER_RECORDS - first;
    uint64_t contiguous = count < before_wrap ? count : before_wrap;

    uint8_t* records = dst + sizeof(chunk);
    string::memcpy(dst, &chunk, sizeof(chunk));
    string::memcpy(records, &ring.records[first], contiguous * sizeof(trace_record));
    string::memcpy(records + contiguous * sizeof(trace_record), ring.records,
                   (count - contiguous) * sizeof(trace_record));

    ring.lost_reported = lost;
    ring.drained.store_release(start + count);

    return sizeof(chunk) + count * sizeof(trace_record);
}

static bool all_rings_drained(uint32_t cpu_count) {
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        trace_ring& ring = per_cpu_on(ktrace_percpu_ring, cpu);
        uint64_t reserved = ring.head.load_acquire() & ~RING_CLOSED;

        bool records_left = ring.drained.load_relaxed() != reserved;
        bool losses_unreported = ring.lost.load_relaxed() != ring.lost_reported;
        if (records_left || losses_unreported) {
            return false;
        }
    }

    return true;
}

static int32_t wait_for_next_drain() {
    while (true) {
        uint32_t state;
        bool started;

        {
            sync::lock_guard guard(g_session_lock);

            state = g_session_state.load_relaxed();
            started = g_reader.session_started;
        }

        uint64_t wait_ns = DRAIN_INTERVAL_NS;
        if (started) {
            if (state == SESSION_STOPPED || !g_reader.header_sent || g_reader.last_read_was_full) {
                return fs::OK;
            }

            uint64_t since_drain = clock::now_ns() - g_reader.last_drain_ns;
            if (since_drain >= DRAIN_INTERVAL_NS) {
                return fs::OK;
            }

            wait_ns = DRAIN_INTERVAL_NS - since_drain;
        }

        if (sched::sleep_ns(wait_ns) != 0) {
            return fs::ERR_INTR;
        }
    }
}

static ssize_t read_stream(uint8_t* dst, size_t count) {
    if (g_reader.end_sent) {
        return 0;
    }

    while (true) {
        int32_t rc = wait_for_next_drain();
        if (rc != fs::OK) {
            return rc;
        }

        size_t pos = 0;
        if (!g_reader.header_sent) {
            file_header header = build_file_header();
            string::memcpy(dst, &header, sizeof(header));

            pos = sizeof(header);
            g_reader.header_sent = true;
        }

        uint32_t cpu_count = smp::cpu_count();

        for (uint32_t i = 0; i < cpu_count; i++) {
            uint32_t cpu = (g_reader.drain_start_cpu + i) % cpu_count;

            trace_ring& ring = per_cpu_on(ktrace_percpu_ring, cpu);
            pos += drain_ring_chunk(dst + pos, count - pos, cpu, ring);
        }

        g_reader.drain_start_cpu = (g_reader.drain_start_cpu + 1) % cpu_count;

        bool stopped = g_session_state.load_acquire() == SESSION_STOPPED;
        if (stopped && count - pos >= sizeof(chunk_header) && all_rings_drained(cpu_count)) {
            chunk_header end_chunk = {};
            end_chunk.kind = CHUNK_END;
            end_chunk.stop_ns = g_session_stop_ns.load_relaxed();

            string::memcpy(dst + pos, &end_chunk, sizeof(end_chunk));
            pos += sizeof(end_chunk);

            g_reader.end_sent = true;
        }

        g_reader.last_read_was_full = !g_reader.end_sent && count - pos < SMALLEST_CHUNK_BYTES;
        if (!g_reader.last_read_was_full) {
            g_reader.last_drain_ns = clock::now_ns();
        }

        if (pos > 0) {
            g_bytes_streamed.fetch_add_relaxed(pos);
            return static_cast<ssize_t>(pos);
        }
    }
}

int32_t trace_node::open(fs::file*, uint32_t) {
    sync::lock_guard guard(g_session_lock);

    if (g_trace_open_count != 0) {
        return fs::ERR_BUSY;
    }

    g_trace_open_count = 1;
    g_reader = {};

    return fs::OK;
}

int32_t trace_node::on_close(fs::file*) {
    bool recording;

    {
        sync::lock_guard guard(g_session_lock);

        g_trace_open_count = 0;
        recording = g_session_state.load_relaxed() == SESSION_RECORDING;
    }

    // Nothing else drains the rings, so the session ends with its reader
    if (recording) {
        stop();
    }

    return fs::OK;
}

ssize_t trace_node::read(fs::file*, void* buf, size_t count, uint32_t) {
    if (count < SMALLEST_CHUNK_BYTES) {
        return fs::ERR_INVAL;
    }

    {
        sync::lock_guard guard(g_session_lock);

        if (g_reader.read_in_progress) {
            return fs::ERR_BUSY;
        }

        g_reader.read_in_progress = true;
    }

    ssize_t rc = read_stream(static_cast<uint8_t*>(buf), count);

    sync::lock_guard guard(g_session_lock);

    g_reader.read_in_progress = false;
    return rc;
}

__PRIVILEGED_CODE static int32_t add_node(fs::node* dir, fs::node* node, const char* path) {
    if (!node) {
        log::error("ktrace: failed to allocate %s", path);
        return ERR_NO_MEMORY;
    }

    if (devfs::add_char_device_at(dir, node) != devfs::OK) {
        log::error("ktrace: failed to register %s", path);

        heap::kfree_delete(node);
        return ERR_DEVFS;
    }

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_devfs_nodes() {
#if !defined(KTRACE_ENABLED) || KTRACE_ENABLED == 0
    return OK;
#endif

    fs::node* dir = devfs::ensure_dir("ktrace");
    if (!dir) {
        log::error("ktrace: failed to create /dev/ktrace");
        return ERR_DEVFS;
    }

    int32_t rc = add_node(dir, heap::kalloc_new<control_node>(), "/dev/ktrace/control");
    if (rc != OK) {
        return rc;
    }

    auto* status = heap::kalloc_new<devfs::text_snapshot_node>("status", generate_status);
    rc = add_node(dir, status, "/dev/ktrace/status");
    if (rc != OK) {
        return rc;
    }

    return add_node(dir, heap::kalloc_new<trace_node>(), "/dev/ktrace/trace");
}

int32_t start(uint64_t event_mask) {
    sync::lock_guard guard(g_session_lock);

    uint32_t state = g_session_state.load_acquire();
    if (state != SESSION_IDLE && state != SESSION_STOPPED) {
        return ERR_BUSY;
    }

    if (g_trace_open_count == 0 || g_reader.session_started) {
        return ERR_NO_READER;
    }

    uint32_t cpu_count = smp::cpu_count();

    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        trace_ring& ring = per_cpu_on(ktrace_percpu_ring, cpu);

        // The counters are zeroed before the head reopens, so a writer
        // that reserves right away sees an empty ring and keeps its commit.
        ring.committed.store_relaxed(0);
        ring.drained.store_relaxed(0);
        ring.lost.store_relaxed(0);
        ring.lost_reported = 0;
        ring.head.store_release(0);
    }

    g_session_start_ns.store_relaxed(clock::now_ns());
    g_session_stop_ns.store_relaxed(0);
    g_session_event_mask.store_relaxed(event_mask);

    g_bytes_streamed.store_relaxed(0);
    g_reader.session_started = true;

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

    g_session_stop_ns.store_relaxed(clock::now_ns());
    g_session_state.store_release(SESSION_STOPPED);
    return OK;
}

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
bool is_recording(uint16_t event_id) {
    return g_session_state.load_acquire() == SESSION_RECORDING && event_id < EVENT_ID_COUNT &&
           (g_session_event_mask.load_relaxed() & (1ull << event_id));
}

void record_event(const trace_record& rec) {
    if (!is_recording(rec.hdr.event_id)) {
        return;
    }

    uint64_t timestamp = clock::now_ns();
    trace_ring& ring = this_cpu(ktrace_percpu_ring);

    if (!ring.records) {
        return;
    }

    // Atomically reserve a slot in the ring buffer
    uint64_t slot = ring.head.load_acquire();
    do {
        if (slot & RING_CLOSED) {
            return;
        }

        if (slot - ring.drained.load_acquire() >= PERCPU_RECORD_BUFFER_RECORDS) {
            ring.lost.fetch_add_relaxed(1);
            return;
        }
    } while (!ring.head.cmpxchg_weak_acquire(slot, slot + 1));

    // Insert the event record
    trace_record& entry = ring.records[slot % PERCPU_RECORD_BUFFER_RECORDS];
    entry = rec;
    entry.hdr.timestamp = timestamp;

    // Mark the record as committed and make it visible to the other CPUs
    ring.committed.fetch_add_release(1);
}
#endif
} // namespace ktrace
