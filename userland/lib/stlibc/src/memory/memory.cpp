#include <stlibc/memory/memory.h>

extern "C" {

void* memcpy(void* dest, const void* src, size_t n) {
    // Use word-sized operations for better performance
    uint64_t* d = static_cast<uint64_t*>(dest);
    const uint64_t* s = static_cast<const uint64_t*>(src);
    
    // Copy word by word
    while (n >= sizeof(uint64_t)) {
        *d++ = *s++;
        n -= sizeof(uint64_t);
    }
    
    // Handle remaining bytes
    uint8_t* d8 = reinterpret_cast<uint8_t*>(d);
    const uint8_t* s8 = reinterpret_cast<const uint8_t*>(s);
    while (n--) {
        *d8++ = *s8++;
    }
    
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    // If source and destination overlap, we need to copy backwards
    if (dest < src) {
        return memcpy(dest, src, n);
    }
    
    // Copy backwards to handle overlapping regions
    uint8_t* d = static_cast<uint8_t*>(dest) + n;
    const uint8_t* s = static_cast<const uint8_t*>(src) + n;
    
    while (n--) {
        *--d = *--s;
    }
    
    return dest;
}

void* memset(void* dest, int c, size_t n) {
    // Convert c to uint8_t to ensure we're working with a byte
    uint8_t value = static_cast<uint8_t>(c);
    
    // Create a word with the byte value repeated
    uint64_t word = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        word = (word << 8) | value;
    }
    
    // Set word by word
    uint64_t* d = static_cast<uint64_t*>(dest);
    while (n >= sizeof(uint64_t)) {
        *d++ = word;
        n -= sizeof(uint64_t);
    }
    
    // Handle remaining bytes
    uint8_t* d8 = reinterpret_cast<uint8_t*>(d);
    while (n--) {
        *d8++ = value;
    }
    
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = static_cast<const uint8_t*>(s1);
    const uint8_t* p2 = static_cast<const uint8_t*>(s2);
    
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    
    return 0;
}

} // extern "C"
