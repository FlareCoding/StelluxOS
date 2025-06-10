#ifndef STLIBC_MEMORY_H
#define STLIBC_MEMORY_H

#include <stlibc/stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copies n bytes from memory area src to memory area dest.
 * @param dest Pointer to the destination array
 * @param src Pointer to the source array
 * @param n Number of bytes to copy
 * @return Pointer to dest
 */
void* memcpy(void* dest, const void* src, size_t n);

/**
 * @brief Copies n bytes from memory area src to memory area dest, handling overlapping regions.
 * @param dest Pointer to the destination array
 * @param src Pointer to the source array
 * @param n Number of bytes to copy
 * @return Pointer to dest
 */
void* memmove(void* dest, const void* src, size_t n);

/**
 * @brief Fills the first n bytes of the memory area pointed to by dest with the constant byte c.
 * @param dest Pointer to the destination array
 * @param c Value to be set
 * @param n Number of bytes to be set
 * @return Pointer to dest
 */
void* memset(void* dest, int c, size_t n);

/**
 * @brief Compares the first n bytes of memory areas s1 and s2.
 * @param s1 Pointer to the first memory area
 * @param s2 Pointer to the second memory area
 * @param n Number of bytes to compare
 * @return <0 if s1 < s2, 0 if s1 == s2, >0 if s1 > s2
 */
int memcmp(const void* s1, const void* s2, size_t n);

#ifdef __cplusplus
}
#endif

#endif // STLIBC_MEMORY_H
