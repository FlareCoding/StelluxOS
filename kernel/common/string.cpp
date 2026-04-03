#include "string.h"

extern "C" void* memset(void* dest, int c, size_t n) {
    auto* d = static_cast<uint8_t*>(dest);
    for (size_t i = 0; i < n; ++i) {
        d[i] = static_cast<uint8_t>(c);
    }
    return dest;
}

namespace string {

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}

void* memcpy(void* dest, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void* memset(void* dest, int c, size_t n) {
    return ::memset(dest, c, n);
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const auto* p1 = static_cast<const uint8_t*>(s1);
    const auto* p2 = static_cast<const uint8_t*>(s2);
    for (size_t i = 0; i < n; ++i) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }
    return 0;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return static_cast<int>(static_cast<uint8_t>(*s1)) -
           static_cast<int>(static_cast<uint8_t>(*s2));
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return static_cast<int>(static_cast<uint8_t>(s1[i])) -
                   static_cast<int>(static_cast<uint8_t>(s2[i]));
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

size_t strnlen(const char* s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len] != '\0') {
        ++len;
    }
    return len;
}

} // namespace string
