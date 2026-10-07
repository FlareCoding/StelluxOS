#include "trace/ktrace_events.h"
#include "common/string.h"

namespace ktrace {

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1

static void copy_task_name(char* dst, const char* name) {
    string::memcpy(dst, name, string::strnlen(name, TASK_NAME_BYTES));
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

void record_sched_wakeup(
    uint32_t woken_tid,
    const char* woken_name,
    uint32_t target_cpu,
    uint32_t waker_tid,
    const char* waker_name,
    bool from_interrupt
) {
    if (!is_recording(EVENT_SCHED_WAKEUP)) {
        return;
    }

    sched_wakeup_payload payload = {};
    payload.woken_tid = woken_tid;
    payload.target_cpu = target_cpu;
    payload.waker_tid = waker_tid;
    payload.from_interrupt = from_interrupt;
    copy_task_name(payload.woken_name, woken_name);
    copy_task_name(payload.waker_name, waker_name);

    trace_record rec = {};
    rec.hdr.event_id = EVENT_SCHED_WAKEUP;
    rec.hdr.length = sizeof(trace_record) / sizeof(uint64_t);
    string::memcpy(rec.payload, &payload, sizeof(payload));
    record_event(rec);
}

void record_syscall(
    uint32_t tid,
    uint32_t number,
    uint64_t duration_ns,
    int64_t result
) {
    if (!is_recording(EVENT_SYSCALL)) {
        return;
    }

    syscall_payload payload = {};
    payload.duration_ns = duration_ns;
    payload.result = result;
    payload.number = number;
    payload.tid = tid;

    trace_record rec = {};
    rec.hdr.event_id = EVENT_SYSCALL;
    rec.hdr.length = sizeof(trace_record) / sizeof(uint64_t);
    string::memcpy(rec.payload, &payload, sizeof(payload));
    record_event(rec);
}

#endif

} // namespace ktrace
