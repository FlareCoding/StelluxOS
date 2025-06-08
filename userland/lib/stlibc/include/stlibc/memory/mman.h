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

// Memory mapping functions
void* mmap(void* addr, size_t length, int prot_flags, long offset);
int munmap(void* addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif // STELLUX_MMAN_H 