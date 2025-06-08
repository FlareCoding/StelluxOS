#ifndef STLIBC_H
#define STLIBC_H

#include <ctypes.h>

#define SYSCALL_SYS_WRITE       0
#define SYSCALL_SYS_READ        1
#define SYSCALL_SYS_EXIT        2

#define SYSCALL_SYS_ELEVATE     90

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
size_t strlen(const char *str);

int sys_write(const char *str);
void sys_exit(int status);

int printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // STLIBC_H
