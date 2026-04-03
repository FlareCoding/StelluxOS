#include <stlx/cond.h>
#include <stlx/futex.h>

void stlx_cond_wait(stlx_cond_t* cv, stlx_mutex_t* m) {
    uint32_t seq = __atomic_load_n(&cv->seq, __ATOMIC_RELAXED);
    stlx_mutex_unlock(m);
    stlx_futex_wait(&cv->seq, seq, 0);
    stlx_mutex_lock(m);
}

void stlx_cond_signal(stlx_cond_t* cv) {
    __atomic_fetch_add(&cv->seq, 1, __ATOMIC_RELEASE);
    stlx_futex_wake(&cv->seq, 1);
}

void stlx_cond_broadcast(stlx_cond_t* cv) {
    __atomic_fetch_add(&cv->seq, 1, __ATOMIC_RELEASE);
    stlx_futex_wake_all(&cv->seq);
}
