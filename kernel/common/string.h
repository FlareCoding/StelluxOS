#ifndef STELLUX_COMMON_STRING_H
#define STELLUX_COMMON_STRING_H

#include "types.h"

namespace string {

/**
 * @brief Calculate the length of a null-terminated string.
 */
size_t strlen(const char* s);

/**
 * @brief Copy memory from src to dest.
 */
void* memcpy(void* dest, const void* src, size_t n);

/**
 * @brief Fill memory with a constant byte.
 */
void* memset(void* dest, int c, size_t n);

/**
 * @brief Compare two memory regions.
 */
int memcmp(const void* s1, const void* s2, size_t n);

/**
 * @brief Compare two null-terminated strings.
 * @return 0 if equal, negative if s1 < s2, positive if s1 > s2.
 */
int strcmp(const char* s1, const char* s2);

} // namespace string

#endif // STELLUX_COMMON_STRING_H
