#ifndef STLIBC_MALLOC_H
#define STLIBC_MALLOC_H

#include <stlibc/stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocates a block of memory of specified size.
 * @param size Size of the memory block to allocate in bytes.
 * @return Pointer to the allocated memory block, or NULL if allocation fails.
 */
void* malloc(size_t size);

/**
 * @brief Frees a previously allocated memory block.
 * @param ptr Pointer to the memory block to free.
 */
void free(void* ptr);

/**
 * @brief Reallocates a memory block to a new size.
 * @param ptr Pointer to the previously allocated memory block.
 * @param size New size of the memory block in bytes.
 * @return Pointer to the reallocated memory block, or NULL if reallocation fails.
 */
void* realloc(void* ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif // STLIBC_MALLOC_H
