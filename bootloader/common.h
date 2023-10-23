#ifndef COMMON_H
#define COMMON_H

#include <efi.h>
#include <efilib.h>

#define PAGE_SIZE 0x1000

static inline int strncmp(const char *s1, const char *s2, UINT64 n) {
    for (UINT64 i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

#endif // COMMON_H
