#ifndef STLXSTD_CONDITION_VARIABLE_H
#define STLXSTD_CONDITION_VARIABLE_H

#include <stlx/cond.h>
#include <stlxstd/mutex.h>

namespace stlxstd {

class condition_variable {
public:
    condition_variable() : m_cond(STLX_COND_INIT) {}
    ~condition_variable() = default;

    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    void wait(unique_lock<mutex>& lock) {
        stlx_cond_wait(&m_cond, lock.mutex_ptr()->native());
    }

    template<typename Pred>
    void wait(unique_lock<mutex>& lock, Pred pred) {
        while (!pred()) wait(lock);
    }

    void notify_one() { stlx_cond_signal(&m_cond); }
    void notify_all() { stlx_cond_broadcast(&m_cond); }

private:
    stlx_cond_t m_cond;
};

} // namespace stlxstd

#endif // STLXSTD_CONDITION_VARIABLE_H
