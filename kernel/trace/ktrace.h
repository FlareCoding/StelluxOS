#ifndef STELLUX_TRACE_KTRACE_H
#define STELLUX_TRACE_KTRACE_H

#include "common/types.h"

/*
 * Layout of a .ktrace file, which holds one stopped session. Fields are
 * little-endian, offsets count bytes from the start of the file, and every
 * section starts on a 64-byte boundary.
 *
 * File header, 64 bytes:
 *   0   magic             char[8]     "STLXKTRC"
 *   8   version           uint16_t    1
 *   10  header_size       uint16_t    64
 *   12  arch              uint8_t     1 is x86_64, 2 is aarch64
 *   13  reserved          uint8_t[3]
 *   16  cpu_count         uint32_t    entries in the CPU table
 *   20  record_size       uint32_t    sizeof(trace_record)
 *   24  cpu_table_offset  uint64_t
 *   32  boot_unix_ns      uint64_t    wall-clock time at boot, 0 without an RTC
 *   40  session_start_ns  uint64_t    nanoseconds since boot
 *   48  session_stop_ns   uint64_t    nanoseconds since boot
 *   56  reserved          uint8_t[8]
 *
 * CPU table, one 32-byte entry per online CPU in CPU id order:
 *   0   cpu_id            uint32_t
 *   4   flags             uint32_t    bit 0: the ring wrapped
 *                                     bit 1: the CPU has no ring
 *   8   record_count      uint64_t
 *   16  records_offset    uint64_t
 *   24  reserved          uint8_t[8]
 *
 * A CPU's records are `record_count` consecutive `trace_record` structs in
 * slot order, oldest first. An interrupt can record between a writer's clock
 * read and its slot reservation, so readers sort by timestamp.
 *
 * Fields are only appended to the header, growing `header_size`. `version`
 * changes only for layouts that existing readers cannot parse.
 */

namespace ktrace {

constexpr int32_t OK                = 0;
constexpr int32_t ERR_NO_MEMORY     = -1;
constexpr int32_t ERR_BUSY          = -2;
constexpr int32_t ERR_NOT_RECORDING = -3;

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

/**
 * @brief Initializes the kernel tracing and profiling subsystem for a given CPU.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Starts a session on every CPU, emptying the rings first.
 * @return OK, or ERR_BUSY while a session is recording.
 */
int32_t start();

/**
 * @brief Stops the recording session. The rings keep its records until the next start().
 * @return OK, or ERR_NOT_RECORDING when no session is recording.
 */
int32_t stop();

/**
 * @brief Append a record to this CPU's trace ring, stamped with `clock::now_ns()`.
 * Does nothing unless a session is recording. Any timestamp already in `rec` is
 * replaced. Callable from any context at either privilege level.
 */
#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
    void record_event(const trace_record& rec);
#else
    inline void record_event(const trace_record&) {}
#endif
} // namespace ktrace

#endif // STELLUX_TRACE_KTRACE_H
