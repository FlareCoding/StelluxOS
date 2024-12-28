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
typedef __INT64_TYPE__ ssize_t;
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

typedef uint64_t uintptr_t;
#define NULL    0

#define offsetof(type, member) ((uintptr_t) &(((type *)0)->member))

#if defined(__cplusplus)
    #define EXTERN_C extern "C"
#else
    #define EXTERN_C extern
#endif

#define __force_inline__ inline __attribute__((always_inline))
#define __unused (void)

#define __PRIVILEGED_CODE       __attribute__((section(".ktext")))
#define __PRIVILEGED_DATA       __attribute__((section(".kdata")))
#define __PRIVILEGED_RO_DATA    __attribute__((section(".krodata")))

#define __UNIT_TEST __attribute__((used, section(".unit_test")))
#define __UNIT_TEST_UNUSED __attribute__((used, section(".unit_test_unused")))

/**
 * @macro DECLARE_GLOBAL_OBJECT
 * @brief Declares and initializes a global object with proper constructor invocation.
 * 
 * This macro ensures that global objects of non-POD (Plain Old Data) types are
 * correctly value-initialized by invoking their constructors. In some environments,
 * such as bare-metal kernels, the default initialization of global objects
 * (`type name;`) does not guarantee that the constructor will be called. 
 * This can lead to uninitialized member variables and undefined behavior.
 *
 * By using this macro, the constructor is explicitly called through 
 * value initialization (`type name = type();`), ensuring the object is 
 * fully and properly initialized.
 *
 * @param type The type of the global object.
 * @param name The name of the global object.
 *
 * @note Always use this macro when declaring global objects of classes 
 *       that require constructor invocation to initialize internal state.
 *
 * @example
 * // Declare a global spinlock with proper initialization
 * DECLARE_GLOBAL_OBJECT(spinlock, g_lock);
 */
#define DECLARE_GLOBAL_OBJECT(type, name) type name = type()

#endif
