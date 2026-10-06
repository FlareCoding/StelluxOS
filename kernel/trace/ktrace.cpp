#include "trace/ktrace.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
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

constexpr uint32_t CPU_FLAG_RING_WRAPPED = 1 << 0;
constexpr uint32_t CPU_FLAG_NO_RING      = 1 << 1;

namespace {

struct trace_ring {
    trace_record*          records;
    sync::atomic<uint64_t> head;
    sync::atomic<uint64_t> committed;
};

// /dev/ktrace/control, where each write is one command, `start` or `stop`
class control_node : public fs::node {
public:
    control_node() : fs::node(fs::node_type::char_device, nullptr, "control") {}

    ssize_t write(fs::file*, const void* buf, size_t count, uint32_t) override {
        const char* text = static_cast<const char*>(buf);

        int32_t rc;
        if (is_command(text, count, "start")) {
            rc = start();
        } else if (is_command(text, count, "stop")) {
            rc = stop();
        } else {
            return fs::ERR_INVAL;
        }

        if (rc == ERR_BUSY) {
            return fs::ERR_BUSY;
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

} // anonymous namespace

static DEFINE_PER_CPU_CACHELINE_ALIGNED(trace_ring, ktrace_percpu_ring);

static sync::spinlock g_session_lock = sync::SPINLOCK_INIT;
static sync::atomic<uint32_t> g_session_state;
static sync::atomic<uint64_t> g_session_start_ns;
static sync::atomic<uint64_t> g_session_stop_ns;

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

// `key value` lines, then one `cpu<N> <kept> <flags>` line per CPU with the CPU table flags
static size_t generate_status(char* buf, size_t cap) {
    uint32_t state;
    uint64_t start_ns;
    uint64_t stop_ns;

    {
        sync::lock_guard guard(g_session_lock);

        state = g_session_state.load_acquire();
        start_ns = g_session_start_ns.load_relaxed();
        stop_ns = g_session_stop_ns.load_relaxed();
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
    pos = devfs::append_str(buf, cap, pos, "\n");

    uint32_t cpu_count = smp::cpu_count();

    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        trace_ring& ring = per_cpu_on(ktrace_percpu_ring, cpu);
        uint64_t committed = ring.committed.load_acquire();
        uint64_t kept = committed < PERCPU_RECORD_BUFFER_RECORDS ? committed : PERCPU_RECORD_BUFFER_RECORDS;

        uint32_t flags = 0;
        if (committed > PERCPU_RECORD_BUFFER_RECORDS) {
            flags |= CPU_FLAG_RING_WRAPPED;
        }

        if (!ring.records) {
            flags |= CPU_FLAG_NO_RING;
        }

        pos = devfs::append_str(buf, cap, pos, "cpu");
        pos = devfs::append_u64(buf, cap, pos, cpu);
        pos = devfs::append_str(buf, cap, pos, " ");
        pos = devfs::append_u64(buf, cap, pos, kept);
        pos = devfs::append_str(buf, cap, pos, " ");
        pos = devfs::append_u64(buf, cap, pos, flags);
        pos = devfs::append_str(buf, cap, pos, "\n");
    }

    return pos;
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
    return add_node(dir, status, "/dev/ktrace/status");
}

int32_t start() {
    sync::lock_guard guard(g_session_lock);

    uint32_t state = g_session_state.load_acquire();
    if (state != SESSION_IDLE && state != SESSION_STOPPED) {
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

    g_session_start_ns.store_relaxed(clock::now_ns());
    g_session_stop_ns.store_relaxed(0);
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
