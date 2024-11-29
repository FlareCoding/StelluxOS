// memory.h
#ifndef MEMORY_H
#define MEMORY_H
#include <types.h>

/**
 * @brief Sets the first `count` bytes of the memory area pointed to by `ptr` to the specified `value`.
 *
 * @param ptr Pointer to the memory area to be filled.
 * @param value The value to be set. The value is passed as an `int`, but the function fills the memory using the unsigned char conversion of this value.
 * @param count Number of bytes to be set to the value.
 * @return void* Pointer to the memory area `ptr`.
 */
void* memset(void* ptr, int value, size_t count);

/**
 * @brief Copies `count` bytes from the memory area `src` to memory area `dest`.
 *
 * @param dest Pointer to the destination memory area where the content is to be copied.
 * @param src Pointer to the source memory area from which the content is to be copied.
 * @param count Number of bytes to copy.
 * @return void* Pointer to the destination memory area `dest`.
 */
void* memcpy(void* dest, const void* src, size_t count);

/**
 * @brief Compares the first `count` bytes of the memory areas `ptr1` and `ptr2`.
 *
 * @param ptr1 Pointer to the first memory area.
 * @param ptr2 Pointer to the second memory area.
 * @param count Number of bytes to compare.
 * @return int An integer less than, equal to, or greater than zero if the first `count` bytes of `ptr1` is found, respectively, to be less than, to match, or be greater than the first `count` bytes of `ptr2`.
 */
int memcmp(const void* ptr1, const void* ptr2, size_t count);

#define zeromem(vaddr, size) memset(vaddr, 0, size)

#endif // MEMORY_H
