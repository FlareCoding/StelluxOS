#ifndef STRING_H
#define STRING_H
#include <types.h>
#include <cstdarg>

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
 * @brief Appends a portion of one string to another.
 * 
 * This function appends at most `n` characters from the source string (`src`) 
 * to the destination string (`dest`), ensuring that the resulting string is null-terminated.
 * 
 * The destination string must have enough space to hold the resulting string, 
 * and `src` must be null-terminated.
 * 
 * @param dest Pointer to the destination string.
 * @param src Pointer to the source string to be appended.
 * @param n The maximum number of characters to append from `src`.
 * @return char* Pointer to the destination string.
 */
char* strncat(char* dest, const char* src, size_t n);

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
 *   %c  - char
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

/**
 * @brief Converts a Unicode string to a narrow (ASCII) string.
 * 
 * This function takes a Unicode string (assumed to be UTF-16 encoded) and converts it to a 
 * narrow ASCII string, storing the result in the provided buffer. Non-ASCII characters are 
 * replaced with a placeholder character (e.g., '?'). This function operates without 
 * relying on the standard library, making it suitable for kernel-level operations.
 * 
 * @param unicode_string Pointer to the UTF-16 encoded Unicode string to be converted.
 * @param buffer Pointer to the buffer where the resulting narrow string will be stored.
 */
void convert_unicode_to_narrow_string(void* unicode_string, char* buffer);

namespace kstl {
/**
 * @class string
 * @brief Provides a dynamically sized string class with a variety of operations.
 * 
 * This class supports common string manipulation tasks such as concatenation, comparison,
 * and element access. It also provides constructors for various initialization methods.
 */
class string {
public:
    /**
     * @brief Represents a special value indicating no match found in search operations.
     */
    static const size_t npos = static_cast<size_t>(-1);

    /**
     * @brief Constructs an empty string.
     * 
     * Initializes the string to an empty state.
     */
    string();

    /**
     * @brief Destructor for the string class.
     * 
     * Releases any resources held by the string.
     */
    ~string();

    /**
     * @brief Constructs a string from a null-terminated C-string.
     * @param str Pointer to the null-terminated string to initialize with.
     * 
     * Copies the contents of the provided C-string into the string.
     */
    string(const char* str);

    /**
     * @brief Copy constructor for the string class.
     * @param other The string object to copy.
     * 
     * Creates a new string as a copy of the provided string.
     */
    string(const string& other);

    /**
     * @brief Move constructor for the string class.
     * @param other The string object to move.
     * 
     * Transfers ownership of resources from the provided string, leaving it empty.
     */
    string(string&& other);

    /**
     * @brief Copy assignment operator for the string class.
     * @param other The string object to copy.
     * @return Reference to the updated string.
     * 
     * Replaces the current string with a copy of the provided string.
     */
    string& operator=(const string& other);

    /**
     * @brief Concatenates two strings and returns the result.
     * @param other The string to concatenate.
     * @return A new string containing the concatenation of the current and the other string.
     */
    string operator+(const string& other) const;

    /**
     * @brief Appends another string to the current string.
     * @param other The string to append.
     * @return Reference to the updated string.
     * 
     * Adds the contents of the other string to the end of the current string.
     */
    string& operator+=(const string& other);

    /**
     * @brief Accesses a character at the specified index for modification.
     * @param index The index of the character to access.
     * @return Reference to the character at the specified index.
     * 
     * Provides direct access to the character at the given index, allowing modification.
     * The behavior is undefined if the index is out of bounds.
     */
    char& operator[](size_t index);

    /**
     * @brief Accesses a character at the specified index for read-only access.
     * @param index The index of the character to access.
     * @return Reference to the character at the specified index.
     * 
     * Provides read-only access to the character at the given index.
     * The behavior is undefined if the index is out of bounds.
     */
    const char& operator[](size_t index) const;

    /**
     * @brief Compares two strings for equality.
     * @param other The string to compare with.
     * @return True if the strings are equal, false otherwise.
     */
    bool operator==(const string& other) const;

    /**
     * @brief Compares two strings for inequality.
     * @param other The string to compare with.
     * @return True if the strings are not equal, false otherwise.
     */
    bool operator!=(const string& other) const;

    /**
     * @brief Retrieves the length of the string.
     * @return The number of characters in the string.
     * 
     * Returns the current size of the string, excluding the null terminator.
     */
    size_t length() const;

    /**
     * @brief Retrieves the current capacity of the string.
     * @return The number of characters that can be stored without resizing.
     * 
     * The capacity reflects the allocated memory for the string, which may be larger than its length.
     */
    size_t capacity() const;

    /**
     * @brief Appends a null-terminated C-string to the end of the current string.
     * @param str Pointer to the C-string to append.
     * 
     * Adds the characters from the provided C-string to the end of the current string, resizing if necessary.
     */
    void append(const char* str);

    /**
     * @brief Appends a single character to the end of the current string.
     * @param chr The character to append.
     * 
     * Adds the specified character to the end of the string, resizing if necessary.
     */
    void append(char chr);

    /**
     * @brief Reserves memory to accommodate a specified capacity.
     * @param new_capacity The desired capacity for the string.
     * 
     * Increases the capacity of the string to at least `new_capacity` without changing its length.
     * Does nothing if the current capacity is already sufficient.
     */
    void reserve(size_t new_capacity);

    /**
     * @brief Resizes the string to the specified size.
     * @param new_size The new size for the string.
     * 
     * If the new size is larger, the string is expanded with null characters. If smaller, the string is truncated.
     */
    void resize(size_t new_size);

    /**
     * @brief Finds the first occurrence of a character in the string.
     * @param c The character to search for.
     * @return The index of the first occurrence of the character, or `npos` if not found.
     */
    size_t find(char c) const;

    /**
     * @brief Finds the first occurrence of a C-string in the string.
     * @param str Pointer to the null-terminated C-string to search for.
     * @return The index of the first occurrence of the substring, or `npos` if not found.
     */
    size_t find(const char* str) const;

    /**
     * @brief Finds the first occurrence of another string in the current string.
     * @param str The string to search for.
     * @return The index of the first occurrence of the string, or `npos` if not found.
     */
    size_t find(const string& str) const;

    /**
     * @brief Finds the first occurrence of a character in the string starting from a specific index.
     * @param c The character to search for.
     * @param start The starting index for the search.
     * @return The index of the first occurrence of the character, or `npos` if not found.
     */
    size_t find(char c, size_t start) const;

    /**
     * @brief Finds the first occurrence of a C-string in the string starting from a specific index.
     * @param substr Pointer to the null-terminated C-string to search for.
     * @param start The starting index for the search.
     * @return The index of the first occurrence of the substring, or `npos` if not found.
     */
    size_t find(const char* substr, size_t start) const;

    /**
     * @brief Finds the first occurrence of another string in the current string starting from a specific index.
     * @param str The string to search for.
     * @param start The starting index for the search.
     * @return The index of the first occurrence of the string, or `npos` if not found.
     */
    size_t find(const string& str, size_t start) const;

    /**
     * @brief Finds the last occurrence of a character in the string.
     * @param c The character to search for.
     * @return The index of the last occurrence of the character, or `npos` if not found.
     */
    size_t find_last_of(char c) const;

    /**
     * @brief Retrieves a substring of the string.
     * @param start The starting index of the substring.
     * @param length The length of the substring. Defaults to `npos` for the remainder of the string.
     * @return A new string containing the specified substring.
     * 
     * Extracts a substring starting at the specified index. If the length exceeds the remaining characters,
     * the substring will include characters up to the end of the string.
     */
    string substring(size_t start, size_t length = npos) const;

    /**
     * @brief Checks if the string starts with a specific prefix.
     * @param prefix The prefix to check.
     * @return True if the string starts with the prefix, false otherwise.
     */
    bool starts_with(const kstl::string& prefix) const;

    /**
     * @brief Clears the contents of the string.
     * 
     * Sets the string to an empty state, releasing any dynamically allocated memory if necessary.
     */
    void clear();

    /**
     * @brief Checks if the string is empty.
     * @return True if the string has no characters, false otherwise.
     * 
     * This is an inline method for quick checks of string emptiness.
     */
    inline bool empty() const { return m_size == 0; }

    /**
     * @brief Retrieves the internal character data of the string.
     * @return Pointer to the internal character buffer.
     * 
     * Provides direct access to the raw character data stored in the string.
     */
    const char* data() const;

    /**
     * @brief Retrieves a null-terminated version of the string.
     * @return Pointer to a null-terminated C-string representation of the string.
     * 
     * This method ensures compatibility with APIs expecting a null-terminated string.
     */
    const char* c_str() const;

private:
    static const size_t m_sso_size = 15; /** Size of the Small String Optimization (SSO) buffer */

    char m_sso_buffer[m_sso_size + 1] = { 0 }; /** Buffer for storing small strings using SSO */

    char*  m_data;       /** Pointer to dynamically allocated memory for larger strings */
    size_t m_size;       /** Current size of the string (number of characters) */
    size_t m_capacity;   /** Current capacity of the string (allocated space) */
    bool   m_using_sso;  /** Indicates whether the string is using SSO for storage */
};

/**
 * @brief Converts an integer to a string.
 * @param value The integer to convert.
 * @return A string representation of the integer.
 */
string to_string(int value);

/**
 * @brief Converts an unsigned integer to a string.
 * @param value The unsigned integer to convert.
 * @return A string representation of the unsigned integer.
 */
string to_string(unsigned int value);
} // namespace kstl

#endif
