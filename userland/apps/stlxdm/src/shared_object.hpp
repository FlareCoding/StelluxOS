#ifndef STLXDM_SHARED_OBJECT_HPP
#define STLXDM_SHARED_OBJECT_HPP

#include <cstddef>

/* A shared memory object holding `len` bytes of `data`, as the clipboard keeps them, or -1 with errno set */
int make_shared_object(const void* data, size_t len);

#endif
