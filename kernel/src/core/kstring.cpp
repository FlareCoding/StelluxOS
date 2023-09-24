#include "kstring.h"

const char g_hexAlphabet[17] = "0123456789abcdef";

int lltoa(
    uint64_t val,
    char* buffer,
    uint64_t bufsize
) {
    uint8_t length = 0;
    uint64_t lenTest = val;
    while (lenTest / 10 > 0) {
        lenTest /= 10;
        ++length;
    }

    if (bufsize < static_cast<uint64_t>(length + 1)) {
        return -1;
    }

    uint8_t index = 0;
    while (val / 10 > 0) {
        uint8_t remainder = val % 10;
        val /= 10;

        buffer[length - index] = remainder + '0';
        ++index;
    }

    // Last digit
    uint8_t remainder = val % 10;
    buffer[length - index] = remainder + '0';

    // Add the null-terminator
    buffer[length + 1] = 0;

    return 0;
}

int itoa(
    int32_t val,
    char* buffer,
    uint64_t bufsize
) {
    bool negative = false;
    if (val < 0) {
        negative = true;
        val *= -1;
        buffer[0] = '-';
    }

    uint8_t length = 0;
    uint64_t lenTest = val;
    while (lenTest / 10 > 0) {
        lenTest /= 10;
        ++length;
    }

    if (bufsize < static_cast<uint64_t>(negative + length + 1)) {
        return -1;
    }

    uint8_t index = 0;
    while (val / 10 > 0) {
        uint8_t remainder = val % 10;
        val /= 10;

        buffer[negative + length - index] = remainder + '0';
        ++index;
    }

    // Last digit
    uint8_t remainder = val % 10;
    buffer[negative + length - index] = remainder + '0';

    // Add the null-terminator
    buffer[negative + length + 1] = 0;

    return 0;
}

int htoa(
    uint64_t val,
    char* buffer,
    uint64_t bufsize
) {
    if (bufsize < 17) {
        return -1;
    }

    uint8_t idx = 0;
    while (idx < 8) {
        uint8_t* ptr = ((uint8_t*)&val) + idx;
        uint8_t currentByte = *ptr;

        buffer[15 - (idx * 2 + 1)] = g_hexAlphabet[(currentByte & 0xF0) >> 4];
        buffer[15 - (idx * 2 + 0)] = g_hexAlphabet[(currentByte & 0x0F) >> 0];

        ++idx;
    }

    buffer[idx * 2] = 0;
    return 0;
}
