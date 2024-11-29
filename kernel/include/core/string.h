#ifndef STRING_H
#define STRING_H

#include <cstdarg>
#include <types.h>

/**
 * @brief Reverses a string in place.
 * 
 * This function takes a character array and its length, then reverses the characters
 * within the array without allocating additional memory. It is useful for operations
 * that require the string to be read backwards or for certain encoding algorithms.
 * 
 * @param str Pointer to the character array to be reversed.
 * @param length The number of characters in the string.
 */
void reverse_str(char* str, int length);

/**
 * @brief Compares two null-terminated strings.
 * 
 * This function lexicographically compares the characters of two strings. It returns an 
 * integer less than, equal to, or greater than zero if the first string is found, 
 * respectively, to be less than, to match, or be greater than the second string.
 * 
 * @param str1 Pointer to the first null-terminated string.
 * @param str2 Pointer to the second null-terminated string.
 * @return int Negative value if str1 < str2, zero if str1 == str2, positive value if str1 > str2.
 */
int strcmp(const char* str1, const char* str2);

/**
 * @brief Copies a null-terminated string to a destination buffer.
 * 
 * This function copies the content of the source string, including the terminating null byte,
 * to the destination buffer. It ensures that the destination buffer contains an exact copy
 * of the source string, provided that the buffer is large enough to hold the copied string.
 * 
 * @param dest Pointer to the destination buffer where the content is to be copied.
 * @param src Pointer to the null-terminated source string to be copied.
 * @return char* Pointer to the destination string dest.
 */
char* strcpy(char* dest, const char* src);

/**
 * @brief Calculates the length of a null-terminated string.
 * 
 * This function computes the number of characters in a string, excluding the terminating
 * null byte ('\0'). It is commonly used to determine the size of a string before
 * performing operations such as copying or concatenation.
 * 
 * @param str Pointer to the null-terminated string whose length is to be calculated.
 * @return size_t The number of characters in the string.
 */
size_t strlen(const char* str);

/**
 * @brief Converts an unsigned integer to a string representation.
 * 
 * This function converts a 64-bit unsigned integer to its string representation in the
 * specified base (e.g., binary, decimal, hexadecimal). The resulting string is stored
 * in the provided buffer, ensuring that it does not exceed the buffer size.
 * 
 * @param value The unsigned 64-bit integer value to be converted.
 * @param buffer Pointer to the buffer where the resulting string will be stored.
 * @param buffer_size The size of the buffer in bytes.
 * @param base The numerical base for the conversion (e.g., 10 for decimal, 16 for hexadecimal).
 * @return int Returns 0 on successful conversion, or a negative value if an error occurs
 *             (e.g., buffer is too small).
 */
int uint_to_str(uint64_t value, char* buffer, size_t buffer_size, int base);

/**
 * @brief Converts a signed integer to a string representation.
 * 
 * This function converts a 64-bit signed integer to its string representation in the
 * specified base (e.g., binary, decimal, hexadecimal). It handles negative numbers by
 * prefixing the string with a minus sign. The resulting string is stored in the provided
 * buffer, ensuring that it does not exceed the buffer size.
 * 
 * @param value The signed 64-bit integer value to be converted.
 * @param buffer Pointer to the buffer where the resulting string will be stored.
 * @param buffer_size The size of the buffer in bytes.
 * @param base The numerical base for the conversion (e.g., 10 for decimal, 16 for hexadecimal).
 * @return int Returns 0 on successful conversion, or a negative value if an error occurs
 *             (e.g., buffer is too small).
 */
int int_to_str(int64_t value, char* buffer, size_t buffer_size, int base);

/**
 * A safe implementation of sprintf supporting multiple format specifiers.
 *
 * Supported specifiers:
 *   %s  - null-terminated const char* string
 *   %i  - int32_t
 *   %u  - uint32_t
 *   %d  - int32_t
 *   %lli - int64_t
 *   %llu - uint64_t
 *   %x  - uint32_t in hexadecimal
 *   %llx - uint64_t in hexadecimal
 *   %%  - literal '%'
 *
 * @param buffer Pointer to the buffer where the formatted string will be stored.
 * @param buffer_size The size of the buffer to prevent overflows.
 * @param format The format string.
 * @param ... Variable arguments to be formatted.
 * @return The number of characters written, excluding the null terminator.
 */
int sprintf(char* buffer, size_t buffer_size, const char* format, ...);

#endif
