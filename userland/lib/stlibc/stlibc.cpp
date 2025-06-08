#include "stlibc.h"
#include <stdarg.h>

extern "C" {

int syscall(
    uint64_t syscallNumber,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
){
    long ret;
    asm volatile(
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "mov %5, %%r10\n"
        "mov %6, %%r8\n"
        "mov %7, %%r9\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r" (ret)
        : "r"(syscallNumber), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4), "r"(arg5), "r"(arg6)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"
    );
    return static_cast<int>(ret);
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void *memset(void *dest, int c, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    for (size_t i = 0; i < n; ++i) {
        d[i] = (unsigned char)c;
    }
    return dest;
}

size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) {
        ++len;
    }
    return len;
}

static void reverse(char *str, size_t len) {
    size_t i = 0, j = len - 1;
    while (i < j) {
        char tmp = str[i];
        str[i] = str[j];
        str[j] = tmp;
        ++i; --j;
    }
}

static int uint_to_base(uint64_t val, char *buf, size_t bufsize, int base) {
    const char digits[] = "0123456789abcdef";
    size_t i = 0;
    if (val == 0) {
        if (i < bufsize - 1)
            buf[i++] = '0';
    } else {
        while (val && i < bufsize - 1) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }
    if (i >= bufsize) return -1;
    buf[i] = '\0';
    reverse(buf, i);
    return (int)i;
}

static int int_to_base(int64_t val, char *buf, size_t bufsize, int base) {
    if (val < 0) {
        if (bufsize < 2) return -1;
        int len = uint_to_base((uint64_t)(-val), buf + 1, bufsize - 1, base);
        if (len < 0) return -1;
        buf[0] = '-';
        return len + 1;
    }
    return uint_to_base((uint64_t)val, buf, bufsize, base);
}

static int vsnprintf_simple(char *buf, size_t size, const char *fmt, va_list args) {
    size_t idx = 0;
    for (const char *p = fmt; *p && idx < size - 1; ++p) {
        if (*p != '%') {
            buf[idx++] = *p;
            continue;
        }
        ++p;
        if (*p == '%') {
            buf[idx++] = '%';
        } else if (*p == 's') {
            const char *s = va_arg(args, const char *);
            while (*s && idx < size - 1) buf[idx++] = *s++;
        } else if (*p == 'd' || *p == 'i') {
            char num[32];
            int len = int_to_base(va_arg(args, int), num, sizeof(num), 10);
            for (int i = 0; i < len && idx < size - 1; ++i) buf[idx++] = num[i];
        } else if (*p == 'u') {
            char num[32];
            int len = uint_to_base(va_arg(args, unsigned int), num, sizeof(num), 10);
            for (int i = 0; i < len && idx < size - 1; ++i) buf[idx++] = num[i];
        } else if (*p == 'x') {
            char num[32];
            int len = uint_to_base(va_arg(args, unsigned int), num, sizeof(num), 16);
            for (int i = 0; i < len && idx < size - 1; ++i) buf[idx++] = num[i];
        } else {
            buf[idx++] = '%';
            if (idx < size - 1) buf[idx++] = *p;
        }
    }
    buf[idx] = '\0';
    return (int)idx;
}

int printf(const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf_simple(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    sys_write(buffer);
    return len;
}

int sys_write(const char *str) {
    return syscall(SYSCALL_SYS_WRITE, 0, (uint64_t)str, 0, 0, 0, 0);
}

void sys_exit(int status) {
    syscall(SYSCALL_SYS_EXIT, (uint64_t)status, 0, 0, 0, 0, 0);
    while (1) {}
}

} // extern "C"
