#ifndef STELLUX_TRACE_KTRACE_FORMAT_H
#define STELLUX_TRACE_KTRACE_FORMAT_H

// Shared by the kernel and trace readers, so it includes nothing. Includers
// must provide the fixed-width integer types and size_t first.

/*
 * A .ktrace file holds one session, streamed while it records. Fields are
 * little-endian and every section is a multiple of 64 bytes.
 *
 *   +-------------------------+
 *   | file_header             |  64 bytes
 *   +-------------------------+
 *   | chunk_header            |  64 bytes, CHUNK_RECORDS
 *   | trace_record            |  record_count records of one CPU,
 *   | ...                     |  oldest first
 *   +-------------------------+
 *   | ...                     |  more chunks, CPUs interleaved
 *   +-------------------------+
 *   | chunk_header            |  CHUNK_END, last in the file
 *   +-------------------------+
 *
 * A file without a CHUNK_END chunk was cut short. Readers sort records by
 * timestamp, since an interrupt can record between a writer's clock read and
 * its slot reservation, and they drop records older than session_start_ns.
 * Fields are only appended to the headers, and version changes only for
 * incompatible layouts.
 */

namespace ktrace {

constexpr char     FILE_MAGIC[]  = "STLXKTRC";
constexpr uint16_t FILE_VERSION  = 1;

constexpr uint32_t CHUNK_RECORDS = 1;
constexpr uint32_t CHUNK_END     = 2;

// An event mask holds one bit per event id
constexpr uint16_t EVENT_ID_COUNT = 64;
constexpr uint64_t ALL_EVENTS     = ~0ull;

constexpr uint16_t EVENT_SCHED_SWITCH = 1;
constexpr uint16_t EVENT_SCHED_WAKEUP = 2;
constexpr uint16_t EVENT_SYSCALL      = 3;
constexpr uint16_t EVENT_PAGE_FAULT   = 4;

constexpr uint8_t SWITCH_REASON_PREEMPTED = 0;
constexpr uint8_t SWITCH_REASON_YIELDED   = 1;
constexpr uint8_t SWITCH_REASON_BLOCKED   = 2;
constexpr uint8_t SWITCH_REASON_EXITED    = 3;

constexpr uint8_t TASK_KIND_NOT_RECORDED = 0;
constexpr uint8_t TASK_KIND_USER         = 1;
constexpr uint8_t TASK_KIND_KERNEL       = 2;
constexpr uint8_t TASK_KIND_IDLE         = 3;

constexpr size_t TASK_NAME_BYTES = 16;

struct trace_record_header {
    uint64_t    timestamp;     // Nanoseconds since boot, set by record_event()
    uint16_t    event_id;      // ID of the event in a given trace profile
    uint16_t    context;       // Event context information
    uint8_t     length;        // Length of the record in 8-byte words
    uint8_t     flags;         // Record-specific flags
    uint16_t    reserved;
} __attribute__((packed));

static_assert(sizeof(trace_record_header) == 16, "trace_record_header size must be 16 bytes");

struct trace_record {
    trace_record_header hdr; // Event header
    uint64_t            payload[6]; // Event specific payload
} __attribute__((aligned(64)));

static_assert(sizeof(trace_record) == 64, "trace_record size must be 64 bytes");

struct file_header {
    char        magic[8];           // FILE_MAGIC without its terminator
    uint16_t    version;            // FILE_VERSION
    uint16_t    header_size;        // sizeof(file_header)
    uint16_t    arch;               // ELF machine number of the kernel
    uint16_t    reserved0;
    uint32_t    cpu_count;
    uint32_t    record_size;        // sizeof(trace_record)
    uint64_t    boot_unix_ns;       // Wall-clock time at boot, 0 without an RTC
    uint64_t    session_start_ns;   // Nanoseconds since boot
    uint64_t    event_mask;         // Event ids the session recorded, one bit each
    uint64_t    reserved1[2];
} __attribute__((packed));

static_assert(sizeof(file_header) == 64, "file_header size must be 64 bytes");

struct chunk_header {
    uint32_t    kind;               // CHUNK_RECORDS or CHUNK_END
    uint32_t    cpu_id;
    uint64_t    record_count;
    uint64_t    lost_records;       // Records the CPU dropped since its previous chunk
    uint64_t    stop_ns;            // Nanoseconds since boot, set in CHUNK_END
    uint64_t    reserved[4];
} __attribute__((packed));

static_assert(sizeof(chunk_header) == 64, "chunk_header size must be 64 bytes");

struct sched_switch_payload {
    uint32_t    prev_tid;
    uint32_t    next_tid;
    uint8_t     reason;
    uint8_t     prev_kind;
    uint8_t     next_kind;
    uint8_t     reserved;
    uint32_t    next_pid;   // 0 when the task belongs to no process
    char        prev_name[TASK_NAME_BYTES];
    char        next_name[TASK_NAME_BYTES];
} __attribute__((packed));

static_assert(sizeof(sched_switch_payload) == sizeof(trace_record::payload),
              "sched_switch_payload must fill a record payload");

struct sched_wakeup_payload {
    uint32_t    woken_tid;
    uint32_t    target_cpu;
    uint32_t    waker_tid;
    uint8_t     woken_kind;
    uint8_t     waker_kind;
    uint8_t     reserved[2];
    char        woken_name[TASK_NAME_BYTES];
    char        waker_name[TASK_NAME_BYTES];
} __attribute__((packed));

static_assert(sizeof(sched_wakeup_payload) == sizeof(trace_record::payload),
              "sched_wakeup_payload must fill a record payload");

struct syscall_payload {
    uint64_t    duration_ns;
    int64_t     result;
    uint32_t    number;
    uint32_t    tid;
    uint32_t    pid;
    uint8_t     reserved[20];
} __attribute__((packed));

static_assert(sizeof(syscall_payload) == sizeof(trace_record::payload),
              "syscall_payload must fill a record payload");

struct page_fault_payload {
    uint64_t    address;
    uint64_t    flags;
    uint64_t    duration_ns;
    int32_t     result;
    uint32_t    tid;
    uint8_t     reserved[16];
} __attribute__((packed));

static_assert(sizeof(page_fault_payload) == sizeof(trace_record::payload),
              "page_fault_payload must fill a record payload");

} // namespace ktrace

#endif // STELLUX_TRACE_KTRACE_FORMAT_H
