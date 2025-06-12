#ifndef STELLUX_MMAN_H
#define STELLUX_MMAN_H

#include <stlibc/stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Memory protection flags
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

// Memory mapping flags
#define MAP_PRIVATE   0x1
#define MAP_SHARED    0x2
#define MAP_ANONYMOUS 0x4
#define MAP_FIXED     0x8

// Memory mapping functions
void* mmap(void* addr, size_t length, int prot_flags, int flags, long offset);
int munmap(void* addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif // STELLUX_MMAN_H 