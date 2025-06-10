#ifndef STLIBC_FORMAT_H
#define STLIBC_FORMAT_H

#include <stlibc/stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Format flags for printf-style functions
 */
enum format_flags {
    FMT_NONE     = 0x00,  // No flags
    FMT_LEFT     = 0x01,  // Left-justify
    FMT_SIGN     = 0x02,  // Always show sign
    FMT_SPACE    = 0x04,  // Show space for positive numbers
    FMT_ALT      = 0x08,  // Alternative form (#)
    FMT_ZERO     = 0x10,  // Zero-pad
    FMT_UPPER    = 0x20,  // Use uppercase for hex
    FMT_NEGATIVE = 0x40   // Number is negative
};

/**
 * @brief Format specifier structure
 */
struct format_spec {
    int flags;           // Format flags
    int width;           // Minimum field width
    int precision;       // Precision for floating point
    char type;           // Conversion type (d, i, u, x, X, o, s, c, etc.)
    bool has_width;      // Whether width was specified
    bool has_precision;  // Whether precision was specified
};

/**
 * @brief Writes formatted output to stdout
 * @param format Format string
 * @param ... Variable arguments
 * @return Number of characters written
 */
int printf(const char* format, ...);

/**
 * @brief Writes formatted output to a string with size limit
 * @param str Pointer to the destination string
 * @param size Maximum number of bytes to write
 * @param format Format string
 * @param ... Variable arguments
 * @return Number of characters that would have been written if size had been sufficiently large
 */
int snprintf(char* str, size_t size, const char* format, ...);

/**
 * @brief Writes formatted output to a string
 * @param str Pointer to the destination string
 * @param format Format string
 * @param ... Variable arguments
 * @return Number of characters written
 */
int sprintf(char* str, const char* format, ...);

/**
 * @brief Writes formatted output to a string with size limit using va_list
 * @param str Pointer to the destination string
 * @param size Maximum number of bytes to write
 * @param format Format string
 * @param args Variable argument list
 * @return Number of characters that would have been written if size had been sufficiently large
 */
int vsnprintf(char* str, size_t size, const char* format, va_list args);

/**
 * @brief Internal function to parse a format specifier
 * @param format Pointer to the format string
 * @param spec Pointer to the format specifier structure to fill
 * @return Number of characters consumed from the format string
 */
int _parse_format_spec(const char* format, struct format_spec* spec);

/**
 * @brief Internal function to format an integer
 * @param str Pointer to the destination string
 * @param size Maximum number of bytes to write
 * @param value The integer value to format
 * @param spec The format specifier
 * @return Number of characters written
 */
int _format_integer(char* str, size_t size, int64_t value, const struct format_spec* spec);

/**
 * @brief Internal function to format an unsigned integer
 * @param str Pointer to the destination string
 * @param size Maximum number of bytes to write
 * @param value The unsigned integer value to format
 * @param spec The format specifier
 * @return Number of characters written
 */
int _format_unsigned(char* str, size_t size, uint64_t value, const struct format_spec* spec);

/**
 * @brief Internal function to format a string
 * @param str Pointer to the destination string
 * @param size Maximum number of bytes to write
 * @param value The string to format
 * @param spec The format specifier
 * @return Number of characters written
 */
int _format_string(char* str, size_t size, const char* value, const struct format_spec* spec);

/**
 * @brief Internal function to format a character
 * @param str Pointer to the destination string
 * @param size Maximum number of bytes to write
 * @param value The character to format
 * @param spec The format specifier
 * @return Number of characters written
 */
int _format_char(char* str, size_t size, char value, const struct format_spec* spec);

#ifdef __cplusplus
}
#endif

#endif // STLIBC_FORMAT_H
