#ifndef STELLUX_TRACE_KTRACE_EVENTS_H
#define STELLUX_TRACE_KTRACE_EVENTS_H

#include "trace/ktrace.h"

namespace ktrace {

constexpr uint16_t EVENT_SCHED_SWITCH = 1;
constexpr uint16_t EVENT_SCHED_WAKEUP = 2;
constexpr uint16_t EVENT_SYSCALL      = 3;
constexpr uint16_t EVENT_PAGE_FAULT   = 4;

constexpr uint8_t SWITCH_REASON_PREEMPTED = 0;
constexpr uint8_t SWITCH_REASON_YIELDED   = 1;
constexpr uint8_t SWITCH_REASON_BLOCKED   = 2;
constexpr uint8_t SWITCH_REASON_EXITED    = 3;

constexpr size_t TASK_NAME_BYTES = 16;

struct sched_switch_payload {
    uint32_t    prev_tid;
    uint32_t    next_tid;
    uint8_t     reason;
    uint8_t     reserved[7];
    char        prev_name[TASK_NAME_BYTES];
    char        next_name[TASK_NAME_BYTES];
} __attribute__((packed));

static_assert(sizeof(sched_switch_payload) == sizeof(trace_record::payload),
              "sched_switch_payload must fill a record payload");

struct sched_wakeup_payload {
    uint32_t    woken_tid;
    uint32_t    target_cpu;
    uint32_t    waker_tid;
    uint8_t     reserved[4];
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

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
    /**
    * @brief Records this CPU switching from task `prev_tid` to `next_tid` when the session
    * records EVENT_SCHED_SWITCH.
    */
    void record_sched_switch(uint32_t prev_tid, const char* prev_name,
                             uint32_t next_tid, const char* next_name, uint8_t reason);

    /**
    * @brief Records task `woken_tid` becoming runnable on `target_cpu`.
    */
    void record_sched_wakeup(uint32_t woken_tid, const char* woken_name, uint32_t target_cpu,
                             uint32_t waker_tid, const char* waker_name);

    /**
    * @brief Records one completed syscall of task `tid`.
    */
    void record_syscall(uint32_t tid, uint32_t pid, uint32_t number, uint64_t duration_ns,
                        int64_t result);

    /**
    * @brief Records one page fault of task `tid` at `address`.
    */
    void record_page_fault(uint32_t tid, uint64_t address, uint64_t flags, uint64_t duration_ns,
                           int32_t result);
#else
    inline void record_sched_switch(uint32_t, const char*, uint32_t, const char*, uint8_t) {}
    inline void record_sched_wakeup(uint32_t, const char*, uint32_t, uint32_t, const char*) {}
    inline void record_syscall(uint32_t, uint32_t, uint32_t, uint64_t, int64_t) {}
    inline void record_page_fault(uint32_t, uint64_t, uint64_t, uint64_t, int32_t) {}
#endif

} // namespace ktrace

#endif // STELLUX_TRACE_KTRACE_EVENTS_H
