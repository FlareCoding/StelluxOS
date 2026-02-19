#ifndef STELLUX_COMMON_UTILS_MEMORY_H
#define STELLUX_COMMON_UTILS_MEMORY_H

#include "common/types.h"

namespace memory {

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

} // namespace memory

#endif // STELLUX_COMMON_UTILS_MEMORY_H
