#ifndef STELLUX_SCHED_RUNQUEUE_H
#define STELLUX_SCHED_RUNQUEUE_H

#include "common/types.h"
#include "sync/spinlock.h"

namespace sched {

struct task;
struct sched_policy;

struct runqueue {
    sync::spinlock lock;
    uint32_t       nr_running;
    task*          idle_task;
    sched_policy*  policy;
};

} // namespace sched

#endif // STELLUX_SCHED_RUNQUEUE_H
