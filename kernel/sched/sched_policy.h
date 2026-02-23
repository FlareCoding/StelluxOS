#ifndef STELLUX_SCHED_SCHED_POLICY_H
#define STELLUX_SCHED_SCHED_POLICY_H

#include "sched/task.h"

namespace sched {

struct sched_policy {
    virtual void   enqueue(task* t) = 0;
    virtual void   dequeue(task* t) = 0;
    virtual task*  pick_next() = 0;
    virtual void   tick(task* current) = 0;
protected:
    ~sched_policy() = default;
};

class round_robin_policy : public sched_policy {
public:
    void init();

    void   enqueue(task* t) override;
    void   dequeue(task* t) override;
    task*  pick_next() override;
    void   tick(task*) override {}

private:
    list::head<task, &task::sched_link> m_ready_list;
};

} // namespace sched

#endif // STELLUX_SCHED_SCHED_POLICY_H
