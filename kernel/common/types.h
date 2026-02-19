#ifndef STELLUX_COMMON_TYPES_H
#define STELLUX_COMMON_TYPES_H

/* Exact-width integer types */
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

/* Pointer-sized integer types */
typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;

/* Size types */
typedef uint64_t size_t;
typedef int64_t  ssize_t;

/* NULL / nullptr - C++ compatible */
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif

/*
 * Privileged section decorators.
 *
 * Functions and data marked with these decorators are placed in privileged
 * kernel sections (.priv.*) that are only accessible at Ring 0 / EL1.
 * Unprivileged (lowered) kernel or user threads cannot access these.
 *
 * Usage:
 *   __PRIVILEGED_CODE void my_func();     // Privileged function
 *   __PRIVILEGED_DATA int my_var = 0;     // Privileged initialized data
 *   __PRIVILEGED_RODATA const int K = 1;  // Privileged read-only data
 *   __PRIVILEGED_BSS int my_bss;          // Privileged zero-init data
 *
 * IMPORTANT: Every __PRIVILEGED_CODE function MUST include in its docstring:
 *   @note Privilege: **required**
 */
#define __PRIVILEGED_CODE   __attribute__((section(".priv.text")))
#define __PRIVILEGED_DATA   __attribute__((section(".priv.data")))
#define __PRIVILEGED_RODATA __attribute__((section(".priv.rodata")))
#define __PRIVILEGED_BSS    __attribute__((section(".priv.bss")))

#endif // STELLUX_COMMON_TYPES_H
