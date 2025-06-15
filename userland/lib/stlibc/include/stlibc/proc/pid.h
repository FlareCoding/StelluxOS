#ifndef STLIBC_HEAP_H
#define STLIBC_HEAP_H

#include <stlibc/stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef int64_t pid_t;

/**
 * @brief Get the process ID of the calling process.
 * @return The process ID of the calling process.
 */
pid_t getpid();


#ifdef __cplusplus
}
#endif

#endif // STLIBC_HEAP_H 