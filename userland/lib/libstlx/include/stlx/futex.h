#ifndef STLX_FUTEX_H
#define STLX_FUTEX_H

#include <stdint.h>

/* Block if *addr == expected. timeout_ns=0 waits indefinitely.
 * Returns 0 on wake, negative errno on error (-EAGAIN, -ETIMEDOUT). */
int stlx_futex_wait(uint32_t* addr, uint32_t expected, uint64_t timeout_ns);

/* Wake up to count threads waiting on addr. Returns number woken. */
int stlx_futex_wake(uint32_t* addr, uint32_t count);

/* Wake all threads waiting on addr. Returns number woken. */
int stlx_futex_wake_all(uint32_t* addr);

#endif /* STLX_FUTEX_H */
