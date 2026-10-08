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

constexpr uint8_t FIELD_U8        = 1;
constexpr uint8_t FIELD_U32       = 2;
constexpr uint8_t FIELD_U64       = 3;
constexpr uint8_t FIELD_I32       = 4;
constexpr uint8_t FIELD_I64       = 5;
constexpr uint8_t FIELD_TASK_NAME = 6;  // TASK_NAME_BYTES, NUL-terminated only when shorter
constexpr uint8_t FIELD_RESERVED  = 7;

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

// Lets a reader decode every event from this header alone, without code per event
struct event_field {
    const char* name;
    uint8_t     type;               // FIELD_*
    uint16_t    offset;             // Within the payload
    uint16_t    size;
};

struct event_description {
    uint16_t           id;
    const char*        name;
    const event_field* fields;
    size_t             field_count;
};

constexpr bool field_type_fits_size(const event_field& field) {
    switch (field.type) {
    case FIELD_U8:
        return field.size == 1;
    case FIELD_U32:
    case FIELD_I32:
        return field.size == 4;
    case FIELD_U64:
    case FIELD_I64:
        return field.size == 8;
    case FIELD_TASK_NAME:
        return field.size == TASK_NAME_BYTES;
    case FIELD_RESERVED:
        return true;
    default:
        return false;
    }
}

// Whether `fields` describe every byte of the payload in order, each with a type that fits its size
template <size_t N>
constexpr bool fields_describe_payload(const event_field (&fields)[N], size_t payload_size) {
    size_t next_offset = 0;
    for (size_t i = 0; i < N; i++) {
        if (fields[i].offset != next_offset || !field_type_fits_size(fields[i])) {
            return false;
        }

        next_offset += fields[i].size;
    }

    return next_offset == payload_size;
}

// Takes the name, offset and size from one member, so an entry cannot describe the wrong bytes
#define KTRACE_PAYLOAD_FIELD(payload, member, type) \
    { #member, type, __builtin_offsetof(payload, member), sizeof(payload::member) }

constexpr event_field SCHED_SWITCH_FIELDS[] = {
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, prev_tid,  FIELD_U32),
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, next_tid,  FIELD_U32),
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, reason,    FIELD_U8),
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, prev_kind, FIELD_U8),
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, next_kind, FIELD_U8),
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, reserved,  FIELD_RESERVED),
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, next_pid,  FIELD_U32),
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, prev_name, FIELD_TASK_NAME),
    KTRACE_PAYLOAD_FIELD(sched_switch_payload, next_name, FIELD_TASK_NAME),
};

static_assert(fields_describe_payload(SCHED_SWITCH_FIELDS, sizeof(sched_switch_payload)),
              "SCHED_SWITCH_FIELDS must describe every byte of sched_switch_payload");

constexpr event_field SCHED_WAKEUP_FIELDS[] = {
    KTRACE_PAYLOAD_FIELD(sched_wakeup_payload, woken_tid,  FIELD_U32),
    KTRACE_PAYLOAD_FIELD(sched_wakeup_payload, target_cpu, FIELD_U32),
    KTRACE_PAYLOAD_FIELD(sched_wakeup_payload, waker_tid,  FIELD_U32),
    KTRACE_PAYLOAD_FIELD(sched_wakeup_payload, woken_kind, FIELD_U8),
    KTRACE_PAYLOAD_FIELD(sched_wakeup_payload, waker_kind, FIELD_U8),
    KTRACE_PAYLOAD_FIELD(sched_wakeup_payload, reserved,   FIELD_RESERVED),
    KTRACE_PAYLOAD_FIELD(sched_wakeup_payload, woken_name, FIELD_TASK_NAME),
    KTRACE_PAYLOAD_FIELD(sched_wakeup_payload, waker_name, FIELD_TASK_NAME),
};

static_assert(fields_describe_payload(SCHED_WAKEUP_FIELDS, sizeof(sched_wakeup_payload)),
              "SCHED_WAKEUP_FIELDS must describe every byte of sched_wakeup_payload");

constexpr event_field SYSCALL_FIELDS[] = {
    KTRACE_PAYLOAD_FIELD(syscall_payload, duration_ns, FIELD_U64),
    KTRACE_PAYLOAD_FIELD(syscall_payload, result,      FIELD_I64),
    KTRACE_PAYLOAD_FIELD(syscall_payload, number,      FIELD_U32),
    KTRACE_PAYLOAD_FIELD(syscall_payload, tid,         FIELD_U32),
    KTRACE_PAYLOAD_FIELD(syscall_payload, pid,         FIELD_U32),
    KTRACE_PAYLOAD_FIELD(syscall_payload, reserved,    FIELD_RESERVED),
};

static_assert(fields_describe_payload(SYSCALL_FIELDS, sizeof(syscall_payload)),
              "SYSCALL_FIELDS must describe every byte of syscall_payload");

constexpr event_field PAGE_FAULT_FIELDS[] = {
    KTRACE_PAYLOAD_FIELD(page_fault_payload, address,     FIELD_U64),
    KTRACE_PAYLOAD_FIELD(page_fault_payload, flags,       FIELD_U64),
    KTRACE_PAYLOAD_FIELD(page_fault_payload, duration_ns, FIELD_U64),
    KTRACE_PAYLOAD_FIELD(page_fault_payload, result,      FIELD_I32),
    KTRACE_PAYLOAD_FIELD(page_fault_payload, tid,         FIELD_U32),
    KTRACE_PAYLOAD_FIELD(page_fault_payload, reserved,    FIELD_RESERVED),
};

static_assert(fields_describe_payload(PAGE_FAULT_FIELDS, sizeof(page_fault_payload)),
              "PAGE_FAULT_FIELDS must describe every byte of page_fault_payload");

#undef KTRACE_PAYLOAD_FIELD

constexpr event_description EVENTS[] = {
    { EVENT_SCHED_SWITCH, "sched_switch", SCHED_SWITCH_FIELDS, sizeof(SCHED_SWITCH_FIELDS) / sizeof(event_field) },
    { EVENT_SCHED_WAKEUP, "sched_wakeup", SCHED_WAKEUP_FIELDS, sizeof(SCHED_WAKEUP_FIELDS) / sizeof(event_field) },
    { EVENT_SYSCALL,      "syscall",      SYSCALL_FIELDS,      sizeof(SYSCALL_FIELDS) / sizeof(event_field) },
    { EVENT_PAGE_FAULT,   "page_fault",   PAGE_FAULT_FIELDS,   sizeof(PAGE_FAULT_FIELDS) / sizeof(event_field) },
};

// Whether every event has its own id with a bit in the event mask
constexpr bool event_ids_are_distinct_mask_bits() {
    uint64_t seen = 0;
    for (const event_description& event : EVENTS) {
        if (event.id >= EVENT_ID_COUNT || (seen & (1ull << event.id))) {
            return false;
        }

        seen |= 1ull << event.id;
    }

    return true;
}

static_assert(event_ids_are_distinct_mask_bits(), "EVENTS must give each event its own id below EVENT_ID_COUNT");

} // namespace ktrace

#endif // STELLUX_TRACE_KTRACE_FORMAT_H
