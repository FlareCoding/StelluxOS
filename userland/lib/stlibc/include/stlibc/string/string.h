#ifndef STLIBC_STRING_H
#define STLIBC_STRING_H

#include <stlibc/stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Calculates the length of a string.
 * @param str Pointer to the null-terminated string.
 * @return The length of the string in bytes, not including the null terminator.
 * 
 * @note This function does not check for null pointer.
 */
size_t strlen(const char* str);

/**
 * @brief Copies a string.
 * @param dest Pointer to the destination array.
 * @param src Pointer to the source string.
 * @return Pointer to the destination string.
 * 
 * @note This function does not check for buffer overflow.
 * @note The destination must be large enough to contain the source string.
 */
char* strcpy(char* dest, const char* src);

/**
 * @brief Copies a string with length limit.
 * @param dest Pointer to the destination array.
 * @param src Pointer to the source string.
 * @param n Maximum number of bytes to copy.
 * @return Pointer to the destination string.
 * 
 * @note This function will copy at most n bytes from src to dest.
 * @note If src is less than n bytes long, the remainder of dest will be filled with null bytes.
 * @note If src is n or more bytes long, dest will not be null-terminated.
 */
char* strncpy(char* dest, const char* src, size_t n);

/**
 * @brief Concatenates two strings.
 * @param dest Pointer to the destination string.
 * @param src Pointer to the source string.
 * @return Pointer to the destination string.
 * 
 * @note This function does not check for buffer overflow.
 * @note The destination must be large enough to contain both strings.
 */
char* strcat(char* dest, const char* src);

/**
 * @brief Concatenates two strings with length limit.
 * @param dest Pointer to the destination string.
 * @param src Pointer to the source string.
 * @param n Maximum number of bytes to append.
 * @return Pointer to the destination string.
 * 
 * @note This function will append at most n bytes from src to dest.
 * @note The destination must be large enough to contain both strings.
 */
char* strncat(char* dest, const char* src, size_t n);

/**
 * @brief Compares two strings.
 * @param s1 Pointer to the first string.
 * @param s2 Pointer to the second string.
 * @return <0 if s1 < s2, 0 if s1 == s2, >0 if s1 > s2.
 * 
 * @note This function compares strings lexicographically.
 */
int strcmp(const char* s1, const char* s2);

/**
 * @brief Compares two strings with length limit.
 * @param s1 Pointer to the first string.
 * @param s2 Pointer to the second string.
 * @param n Maximum number of bytes to compare.
 * @return <0 if s1 < s2, 0 if s1 == s2, >0 if s1 > s2.
 * 
 * @note This function compares at most n bytes of the strings.
 */
int strncmp(const char* s1, const char* s2, size_t n);

/**
 * @brief Finds the first occurrence of a character in a string.
 * @param str Pointer to the string to search.
 * @param c Character to search for.
 * @return Pointer to the first occurrence of c in str, or NULL if not found.
 */
char* strchr(const char* str, int c);

/**
 * @brief Finds the last occurrence of a character in a string.
 * @param str Pointer to the string to search.
 * @param c Character to search for.
 * @return Pointer to the last occurrence of c in str, or NULL if not found.
 */
char* strrchr(const char* str, int c);

/**
 * @brief Finds the first occurrence of a substring in a string.
 * @param str Pointer to the string to search.
 * @param sub Pointer to the substring to find.
 * @return Pointer to the first occurrence of sub in str, or NULL if not found.
 */
char* strstr(const char* str, const char* sub);

#ifdef __cplusplus
}
#endif

#endif // STLIBC_STRING_H
