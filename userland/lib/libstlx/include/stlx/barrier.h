#ifndef STLX_BARRIER_H
#define STLX_BARRIER_H

#include <stdint.h>

typedef struct {
    uint32_t count;
    uint32_t generation;
    uint32_t total;
} stlx_barrier_t;

void stlx_barrier_init(stlx_barrier_t* b, uint32_t count);

/* Block until all count threads have called barrier_wait. Reusable. */
void stlx_barrier_wait(stlx_barrier_t* b);

#endif /* STLX_BARRIER_H */
