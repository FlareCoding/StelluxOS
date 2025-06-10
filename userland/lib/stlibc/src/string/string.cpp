#include <stlibc/string/string.h>
#include <stlibc/memory/memory.h>

extern "C" {

size_t strlen(const char* str) {
    if (!str) return 0;
    
    const char* s = str;
    while (*s) s++;
    return s - str;
}

char* strcpy(char* dest, const char* src) {
    if (!dest || !src) return dest;
    
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    if (!dest || !src) return dest;
    
    char* d = dest;
    size_t i;
    
    // Copy up to n bytes
    for (i = 0; i < n && src[i]; i++) {
        d[i] = src[i];
    }
    
    // Pad with null bytes if necessary
    for (; i < n; i++) {
        d[i] = '\0';
    }
    
    return dest;
}

char* strcat(char* dest, const char* src) {
    if (!dest || !src) return dest;
    
    // Find end of dest
    char* d = dest;
    while (*d) d++;
    
    // Append src
    while ((*d++ = *src++));
    
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    if (!dest || !src) return dest;
    
    // Find end of dest
    char* d = dest;
    while (*d) d++;
    
    // Append up to n bytes from src
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        d[i] = src[i];
    }
    
    // Ensure null termination
    d[i] = '\0';
    
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    if (!s1 || !s2) {
        if (!s1 && !s2) return 0;
        return s1 ? 1 : -1;
    }
    
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    
    return static_cast<unsigned char>(*s1) - static_cast<unsigned char>(*s2);
}

int strncmp(const char* s1, const char* s2, size_t n) {
    if (!s1 || !s2) {
        if (!s1 && !s2) return 0;
        return s1 ? 1 : -1;
    }
    
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    
    if (n == 0) return 0;
    return static_cast<unsigned char>(*s1) - static_cast<unsigned char>(*s2);
}

char* strchr(const char* str, int c) {
    if (!str) return nullptr;
    
    // Convert c to unsigned char to handle EOF (-1) correctly
    unsigned char uc = static_cast<unsigned char>(c);
    
    while (*str) {
        if (static_cast<unsigned char>(*str) == uc) {
            return const_cast<char*>(str);
        }
        str++;
    }
    
    // Check for null terminator if c is '\0'
    return (uc == '\0') ? const_cast<char*>(str) : nullptr;
}

char* strrchr(const char* str, int c) {
    if (!str) return nullptr;
    
    // Convert c to unsigned char to handle EOF (-1) correctly
    unsigned char uc = static_cast<unsigned char>(c);
    const char* last = nullptr;
    
    while (*str) {
        if (static_cast<unsigned char>(*str) == uc) {
            last = str;
        }
        str++;
    }
    
    // Check for null terminator if c is '\0'
    if (uc == '\0') {
        return const_cast<char*>(str);
    }
    
    return const_cast<char*>(last);
}

char* strstr(const char* str, const char* sub) {
    if (!str || !sub) return nullptr;
    if (!*sub) return const_cast<char*>(str);
    
    const char* s1 = str;
    while (*s1) {
        const char* s2 = sub;
        const char* s1_start = s1;
        
        while (*s1 && *s2 && *s1 == *s2) {
            s1++;
            s2++;
        }
        
        if (!*s2) {
            return const_cast<char*>(s1_start);
        }
        
        s1 = s1_start + 1;
    }
    
    return nullptr;
}

} // extern "C"
