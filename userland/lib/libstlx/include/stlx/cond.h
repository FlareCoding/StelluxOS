#ifndef STLX_COND_H
#define STLX_COND_H

#include <stdint.h>
#include <stlx/mutex.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t seq; } stlx_cond_t;

#define STLX_COND_INIT { 0 }

/* Atomically unlocks the mutex and sleeps until signaled, then re-locks.
 * Callers must re-check the predicate in a loop (spurious wakeups allowed). */
void stlx_cond_wait(stlx_cond_t* cv, stlx_mutex_t* m);

/* Wake one waiter. */
void stlx_cond_signal(stlx_cond_t* cv);

/* Wake all waiters. */
void stlx_cond_broadcast(stlx_cond_t* cv);

#ifdef __cplusplus
}
#endif

#endif /* STLX_COND_H */
