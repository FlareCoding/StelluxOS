#include "trace/ktrace_events.h"
#include "common/string.h"

namespace ktrace {

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1

static void copy_task_name(char* dst, const char* name) {
    string::memcpy(dst, name, string::strnlen(name, SWITCH_NAME_BYTES));
}

void record_sched_switch(
    uint32_t prev_tid,
    const char* prev_name,
    uint32_t next_tid,
    const char* next_name,
    uint8_t reason
) {
    if (!is_recording(EVENT_SCHED_SWITCH)) {
        return;
    }

    sched_switch_payload payload = {};
    payload.prev_tid = prev_tid;
    payload.next_tid = next_tid;
    payload.reason = reason;
    copy_task_name(payload.prev_name, prev_name);
    copy_task_name(payload.next_name, next_name);

    trace_record rec = {};
    rec.hdr.event_id = EVENT_SCHED_SWITCH;
    rec.hdr.length = sizeof(trace_record) / sizeof(uint64_t);
    string::memcpy(rec.payload, &payload, sizeof(payload));
    record_event(rec);
}

#endif

} // namespace ktrace
