#ifndef STELLUX_TRACE_KTRACE_EVENTS_H
#define STELLUX_TRACE_KTRACE_EVENTS_H

#include "trace/ktrace.h"

namespace sched { struct task; }

namespace ktrace {

#if defined(KTRACE_ENABLED) && KTRACE_ENABLED == 1
    /**
    * @brief Records this CPU switching from task `prev` to `next` when the session
    * records EVENT_SCHED_SWITCH.
    */
    void record_sched_switch(const sched::task* prev, const sched::task* next, uint8_t reason);

    /**
    * @brief Records task `woken` becoming runnable on `target_cpu`.
    */
    void record_sched_wakeup(const sched::task* woken, uint32_t target_cpu, const sched::task* waker);

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
    inline void record_sched_switch(const sched::task*, const sched::task*, uint8_t) {}
    inline void record_sched_wakeup(const sched::task*, uint32_t, const sched::task*) {}
    inline void record_syscall(uint32_t, uint32_t, uint32_t, uint64_t, int64_t) {}
    inline void record_page_fault(uint32_t, uint64_t, uint64_t, uint64_t, int32_t) {}
#endif

} // namespace ktrace

#endif // STELLUX_TRACE_KTRACE_EVENTS_H
