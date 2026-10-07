#ifndef STELLUX_TRACE_KTRACE_EVENTS_H
#define STELLUX_TRACE_KTRACE_EVENTS_H

#include "trace/ktrace.h"

namespace ktrace {

constexpr uint16_t EVENT_SCHED_SWITCH = 1;

constexpr uint8_t SWITCH_REASON_PREEMPTED = 0;
constexpr uint8_t SWITCH_REASON_YIELDED   = 1;
constexpr uint8_t SWITCH_REASON_BLOCKED   = 2;
constexpr uint8_t SWITCH_REASON_EXITED    = 3;

constexpr size_t SWITCH_NAME_BYTES = 16;

// Payload of EVENT_SCHED_SWITCH. Names are cut to SWITCH_NAME_BYTES and NUL padded.
struct sched_switch_payload {
    uint32_t    prev_tid;
    uint32_t    next_tid;
    uint8_t     reason;
    uint8_t     reserved[7];
    char        prev_name[SWITCH_NAME_BYTES];
    char        next_name[SWITCH_NAME_BYTES];
} __attribute__((packed));

static_assert(sizeof(sched_switch_payload) == sizeof(trace_record::payload),
              "sched_switch_payload must fill a record payload");

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
    /**
    * @brief Records this CPU switching from task `prev_tid` to `next_tid` when the session
    * records EVENT_SCHED_SWITCH.
    */
    void record_sched_switch(uint32_t prev_tid, const char* prev_name,
                             uint32_t next_tid, const char* next_name, uint8_t reason);
#else
    inline void record_sched_switch(uint32_t, const char*, uint32_t, const char*, uint8_t) {}
#endif

} // namespace ktrace

#endif // STELLUX_TRACE_KTRACE_EVENTS_H
