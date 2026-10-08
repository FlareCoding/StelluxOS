#ifndef STELLUX_TRACE_KTRACE_H
#define STELLUX_TRACE_KTRACE_H

#include "common/types.h"
#include "trace/ktrace_format.h"

namespace ktrace {

constexpr int32_t OK                = 0;
constexpr int32_t ERR_NO_MEMORY     = -1;
constexpr int32_t ERR_BUSY          = -2;
constexpr int32_t ERR_NOT_RECORDING = -3;
constexpr int32_t ERR_DEVFS         = -4;
constexpr int32_t ERR_NO_READER     = -5;

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
 * emptying the rings first. It streams to the reader of /dev/ktrace/trace.
 * @return OK, ERR_BUSY while a session is recording, or ERR_NO_READER when the trace has
 * no reader or its reader already streamed a session.
 */
int32_t start(uint64_t event_mask);

/**
 * @brief Stops the recording session. Returns once every record reserved in the session
 * is completely written, yielding the CPU while it waits. The reader then gets the rest.
 * @return OK, or ERR_NOT_RECORDING when no session is recording.
 */
int32_t stop();

/**
 * @brief Append a record to this CPU's trace ring, stamped with `clock::now_ns()`.
 * Does nothing unless a session is recording `rec`'s event id, and drops the record while
 * the ring is full. Any timestamp already in `rec` is replaced. Callable from any context
 * at either privilege level.
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
