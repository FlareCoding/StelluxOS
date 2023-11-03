#ifndef KSTRING_H
#define KSTRING_H
#include <ktypes.h>

// uint64_t --> string
int lltoa(
    uint64_t val,
    char* buffer,
    uint64_t bufsize
);

// int32_t --> string
int itoa(
    int32_t val,
    char* buffer,
    uint64_t bufsize
);

// uint64_t --> hex string
int htoa(
    uint64_t val,
    char* buffer,
    uint64_t bufsize
);

uint64_t strlen(const char *str);

#endif