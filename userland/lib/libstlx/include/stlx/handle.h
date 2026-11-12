#ifndef STLX_HANDLE_H
#define STLX_HANDLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The rights a handle holds over its object */
#define STLX_RIGHT_READ  (1u << 0)
#define STLX_RIGHT_WRITE (1u << 1)

/* A new handle to the same object holding only `rights`, a nonempty subset of `handle`'s, to pass to a process that
 * must not write. Returns the handle, or -1 with errno EBADF for a bad handle or EINVAL for a right it lacks. */
int stlx_handle_restrict(int handle, uint32_t rights);

#ifdef __cplusplus
}
#endif

#endif /* STLX_HANDLE_H */
