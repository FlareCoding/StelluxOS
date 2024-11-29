#ifndef KTYPES_H
#define KTYPES_H

#ifdef __INT8_TYPE__
typedef __INT8_TYPE__ int8_t;
#endif
#ifdef __INT16_TYPE__
typedef __INT16_TYPE__ int16_t;
#endif
#ifdef __INT32_TYPE__
typedef __INT32_TYPE__ int32_t;
#endif
#ifdef __INT64_TYPE__
typedef __INT64_TYPE__ int64_t;
#endif
#ifdef __UINT8_TYPE__
typedef __UINT8_TYPE__ uint8_t;
#endif
#ifdef __UINT16_TYPE__
typedef __UINT16_TYPE__ uint16_t;
#endif
#ifdef __UINT32_TYPE__
typedef __UINT32_TYPE__ uint32_t;
#endif
#ifdef __UINT64_TYPE__
typedef __UINT64_TYPE__ uint64_t;
typedef __UINT64_TYPE__ size_t;
#endif

typedef void* uintptr_t;
#define NULL    0

#define offsetof(type, member) ((size_t) &(((type *)0)->member))

#if defined(__cplusplus)
    #define EXTERN_C extern "C"
#else
    #define EXTERN_C extern
#endif

#define __force_inline__ inline __attribute__((always_inline))
#define __unused (void)

#define __UNIT_TEST __attribute__((used, section(".unit_test")))
#define __UNIT_TEST_UNUSED __attribute__((used, section(".unit_test_unused")))

#endif