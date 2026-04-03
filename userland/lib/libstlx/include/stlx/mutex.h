#ifndef STLX_MUTEX_H
#define STLX_MUTEX_H

#include <stdint.h>

/* State 0: unlocked, 1: locked (no waiters), 2: locked (with waiters) */
typedef struct { uint32_t state; } stlx_mutex_t;

#define STLX_MUTEX_INIT { 0 }

void stlx_mutex_lock(stlx_mutex_t* m);
void stlx_mutex_unlock(stlx_mutex_t* m);

/* Returns 0 if acquired, -1 if already held. */
int stlx_mutex_trylock(stlx_mutex_t* m);

#endif /* STLX_MUTEX_H */
