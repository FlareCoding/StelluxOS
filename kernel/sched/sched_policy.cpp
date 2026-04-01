#include "sched/sched_policy.h"
#include "common/logging.h"

namespace sched {

void round_robin_policy::init() {
    m_ready_list.init();
}

void round_robin_policy::enqueue(task* t) {
#ifdef DEBUG
    if (!t) {
        log::fatal("sched: attempted to enqueue null task");
    }
    if (t->sched_link.is_linked()) {
        log::fatal("sched: ready-list double enqueue tid=%u name=%s state=%u cpu=%u on_cpu=%u",
                   t->tid, t->name, t->state, t->exec.cpu, t->exec.on_cpu);
    }
#endif
    m_ready_list.push_back(t);
}

void round_robin_policy::dequeue(task* t) {
    m_ready_list.remove(t);
}

task* round_robin_policy::pick_next() {
    return m_ready_list.pop_front();
}

} // namespace sched
