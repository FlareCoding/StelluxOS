#ifndef STELLUX_TRACE_KTRACE_H
#define STELLUX_TRACE_KTRACE_H

#include "common/types.h"

namespace ktrace {

constexpr int32_t OK                = 0;
constexpr int32_t ERR_NO_MEMORY     = -1;

struct trace_record_header {
    uint64_t    timestamp;     // Timestamp of the event
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

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
    void record_event(const trace_record& rec);
#else
    inline void record_event(const trace_record&) {}
#endif
} // namespace ktrace

#endif // STELLUX_TRACE_KTRACE_H
