#include "trace/ktrace_events.h"
#include "common/string.h"
#include "sched/task.h"

namespace ktrace {

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1

static void copy_task_name(char* dst, const char* name) {
    string::memcpy(dst, name, string::strnlen(name, TASK_NAME_BYTES));
}

static uint8_t task_kind(const sched::task* t) {
    if (t->exec.flags & sched::TASK_FLAG_IDLE) {
        return TASK_KIND_IDLE;
    }

    if (t->exec.flags & sched::TASK_FLAG_KERNEL) {
        return TASK_KIND_KERNEL;
    }

    return TASK_KIND_USER;
}

void record_sched_switch(
    const sched::task* prev,
    const sched::task* next,
    uint8_t reason
) {
    if (!is_recording(EVENT_SCHED_SWITCH)) {
        return;
    }

    sched_switch_payload payload = {};
    payload.prev_tid = prev->tid;
    payload.next_tid = next->tid;
    payload.reason = reason;
    payload.prev_kind = task_kind(prev);
    payload.next_kind = task_kind(next);
    payload.next_pid = next->group ? next->group->pid : 0;
    copy_task_name(payload.prev_name, prev->name);
    copy_task_name(payload.next_name, next->name);

    trace_record rec = {};
    rec.hdr.event_id = EVENT_SCHED_SWITCH;
    rec.hdr.length = sizeof(trace_record) / sizeof(uint64_t);
    string::memcpy(rec.payload, &payload, sizeof(payload));
    record_event(rec);
}

void record_sched_wakeup(
    const sched::task* woken,
    uint32_t target_cpu,
    const sched::task* waker
) {
    if (!is_recording(EVENT_SCHED_WAKEUP)) {
        return;
    }

    sched_wakeup_payload payload = {};
    payload.woken_tid = woken->tid;
    payload.target_cpu = target_cpu;
    payload.waker_tid = waker->tid;
    payload.woken_kind = task_kind(woken);
    payload.waker_kind = task_kind(waker);
    copy_task_name(payload.woken_name, woken->name);
    copy_task_name(payload.waker_name, waker->name);

    trace_record rec = {};
    rec.hdr.event_id = EVENT_SCHED_WAKEUP;
    rec.hdr.length = sizeof(trace_record) / sizeof(uint64_t);
    string::memcpy(rec.payload, &payload, sizeof(payload));
    record_event(rec);
}

void record_syscall(
    uint32_t tid,
    uint32_t pid,
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
    payload.pid = pid;

    trace_record rec = {};
    rec.hdr.event_id = EVENT_SYSCALL;
    rec.hdr.length = sizeof(trace_record) / sizeof(uint64_t);
    string::memcpy(rec.payload, &payload, sizeof(payload));
    record_event(rec);
}

void record_page_fault(
    uint32_t tid,
    uint64_t address,
    uint64_t flags,
    uint64_t duration_ns,
    int32_t result
) {
    if (!is_recording(EVENT_PAGE_FAULT)) {
        return;
    }

    page_fault_payload payload = {};
    payload.address = address;
    payload.flags = flags;
    payload.duration_ns = duration_ns;
    payload.result = result;
    payload.tid = tid;

    trace_record rec = {};
    rec.hdr.event_id = EVENT_PAGE_FAULT;
    rec.hdr.length = sizeof(trace_record) / sizeof(uint64_t);
    string::memcpy(rec.payload, &payload, sizeof(payload));
    record_event(rec);
}

#endif

} // namespace ktrace
