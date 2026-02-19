#ifndef STELLUX_COMMON_STRING_H
#define STELLUX_COMMON_STRING_H

#include "core/utils/string.h"
#include "core/utils/memory.h"

namespace string {

/**
 * @brief Compatibility aliases for legacy call sites.
 */
inline void* memcpy(void* dest, const void* src, size_t n) {
    return memory::memcpy(dest, src, n);
}

inline void* memset(void* dest, int c, size_t n) {
    return memory::memset(dest, c, n);
}

inline int memcmp(const void* s1, const void* s2, size_t n) {
    return memory::memcmp(s1, s2, n);
}

} // namespace string

#endif // STELLUX_COMMON_STRING_H
