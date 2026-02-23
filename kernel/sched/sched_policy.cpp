#include "sched/sched_policy.h"

namespace sched {

void round_robin_policy::init() {
    m_ready_list.init();
}

void round_robin_policy::enqueue(task* t) {
    m_ready_list.push_back(t);
}

void round_robin_policy::dequeue(task* t) {
    m_ready_list.remove(t);
}

task* round_robin_policy::pick_next() {
    return m_ready_list.pop_front();
}

} // namespace sched
