#ifndef STELLUX_TRACE_KTRACE_H
#define STELLUX_TRACE_KTRACE_H

#include "common/types.h"

/*
 * A .ktrace file holds one stopped session. Fields are little-endian and
 * offsets are relative to the start of the file. Each section starts on a
 * 64-byte boundary.
 *
 *   offset 0           +-------------------------+
 *                      | file_header             |  64 bytes
 *   cpu_table_offset   +-------------------------+
 *                      | cpu_table_entry CPU 0   |  32 bytes each, cpu_count
 *                      | cpu_table_entry CPU 1   |  entries in CPU id order
 *                      | ...                     |
 *   records_offset     +-------------------------+
 *   of CPU 0           | trace_record            |  record_count records,
 *                      | ...                     |  oldest first
 *   records_offset     +-------------------------+
 *   of CPU 1           | trace_record            |
 *                      | ...                     |
 *                      +-------------------------+
 *
 * Readers sort records by timestamp, since an interrupt can record between a
 * writer's clock read and its slot reservation, and they drop records older
 * than session_start_ns. Fields are only appended to the header, growing
 * header_size, and version changes only for incompatible layouts.
 */

namespace ktrace {

constexpr int32_t OK                = 0;
constexpr int32_t ERR_NO_MEMORY     = -1;
constexpr int32_t ERR_BUSY          = -2;
constexpr int32_t ERR_NOT_RECORDING = -3;
constexpr int32_t ERR_DEVFS         = -4;

constexpr char     FILE_MAGIC[]  = "STLXKTRC";
constexpr uint16_t FILE_VERSION  = 1;

constexpr uint32_t CPU_FLAG_RING_WRAPPED = 1 << 0;
constexpr uint32_t CPU_FLAG_NO_RING      = 1 << 1;

// An event mask holds one bit per event id
constexpr uint16_t EVENT_ID_COUNT = 64;
constexpr uint64_t ALL_EVENTS     = ~0ull;

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
    uint32_t    cpu_count;          // Entries in the CPU table
    uint32_t    record_size;        // sizeof(trace_record)
    uint64_t    cpu_table_offset;
    uint64_t    boot_unix_ns;       // Wall-clock time at boot, 0 without an RTC
    uint64_t    session_start_ns;   // Nanoseconds since boot
    uint64_t    session_stop_ns;    // Nanoseconds since boot
    uint64_t    reserved1;
} __attribute__((packed));

static_assert(sizeof(file_header) == 64, "file_header size must be 64 bytes");

struct cpu_table_entry {
    uint32_t    cpu_id;
    uint32_t    flags;              // CPU_FLAG_* bits
    uint64_t    record_count;
    uint64_t    records_offset;
    uint64_t    reserved;
} __attribute__((packed));

static_assert(sizeof(cpu_table_entry) == 32, "cpu_table_entry size must be 32 bytes");

/**
 * @brief Initializes the kernel tracing and profiling subsystem for a given CPU.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Registers the /dev/ktrace nodes if ktrace is enabled.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_devfs_nodes();

/**
 * @brief Starts a session on every CPU that records the events set in `event_mask`,
 * emptying the rings first.
 * @return OK, or ERR_BUSY while a session is recording.
 */
int32_t start(uint64_t event_mask);

/**
 * @brief Stops the recording session. Returns once every record reserved in the session
 * is completely written, yielding the CPU while it waits. The rings keep the session's
 * records until the next start().
 * @return OK, or ERR_NOT_RECORDING when no session is recording.
 */
int32_t stop();

/**
 * @brief Append a record to this CPU's trace ring, stamped with `clock::now_ns()`.
 * Does nothing unless a session is recording `rec`'s event id. Any timestamp already
 * in `rec` is replaced. Callable from any context at either privilege level.
 */
#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
    void record_event(const trace_record& rec);
#else
    inline void record_event(const trace_record&) {}
#endif

/**
 * @brief Whether a session is recording `event_id`, so typed recorders can return
 * before building a record.
 */
#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
    bool is_recording(uint16_t event_id);
#else
    inline bool is_recording(uint16_t) { return false; }
#endif
} // namespace ktrace

#endif // STELLUX_TRACE_KTRACE_H
