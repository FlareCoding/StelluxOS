#include <stlibc/string/format.h>
#include <stlibc/string/string.h>
#include <stlibc/memory/memory.h>
#include <stlibc/system/syscall.h>
#include <stdarg.h>

extern "C" {

// Internal buffer for number conversion
static char num_buffer[256];

// Helper function to reverse a string in place
static void _reverse_string(char* str, size_t len) {
    char* start = str;
    char* end = str + len - 1;
    while (start < end) {
        char temp = *start;
        *start = *end;
        *end = temp;
        start++;
        end--;
    }
}

// Helper function to convert unsigned number to string
static size_t _utoa(uint64_t value, int base, bool uppercase) {
    if (value == 0) {
        num_buffer[0] = '0';
        return 1;
    }

    size_t i = 0;
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    while (value > 0) {
        num_buffer[i++] = digits[value % base];
        value /= base;
    }

    _reverse_string(num_buffer, i);
    return i;
}

// Helper function to convert signed number to string
static size_t _itoa(int64_t value, int base, bool uppercase) {
    if (value == 0) {
        num_buffer[0] = '0';
        return 1;
    }

    size_t i = 0;
    bool negative = value < 0;
    uint64_t abs_value = negative ? -static_cast<uint64_t>(value) : value;
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    while (abs_value > 0) {
        num_buffer[i++] = digits[abs_value % base];
        abs_value /= base;
    }

    if (negative) {
        num_buffer[i++] = '-';
    }

    _reverse_string(num_buffer, i);
    return i;
}

int _parse_format_spec(const char* format, struct format_spec* spec) {
    const char* start = format;
    format++; // Skip '%'

    // Initialize spec
    spec->flags = FMT_NONE;
    spec->width = 0;
    spec->precision = 0;
    spec->has_width = false;
    spec->has_precision = false;
    spec->is_long_long = false;

    // Parse flags
    while (true) {
        switch (*format) {
            case '-': spec->flags |= FMT_LEFT; break;
            case '+': spec->flags |= FMT_SIGN; break;
            case ' ': spec->flags |= FMT_SPACE; break;
            case '#': spec->flags |= FMT_ALT; break;
            case '0': spec->flags |= FMT_ZERO; break;
            default: goto width;
        }
        format++;
    }

width:
    // Parse width
    if (*format >= '0' && *format <= '9') {
        spec->has_width = true;
        spec->width = 0;
        while (*format >= '0' && *format <= '9') {
            spec->width = spec->width * 10 + (*format - '0');
            format++;
        }
    }

    // Parse precision
    if (*format == '.') {
        format++;
        spec->has_precision = true;
        spec->precision = 0;
        while (*format >= '0' && *format <= '9') {
            spec->precision = spec->precision * 10 + (*format - '0');
            format++;
        }
    }

    // Parse length modifier
    if (*format == 'l' && *(format + 1) == 'l') {
        spec->is_long_long = true;
        format += 2;
    }

    // Parse type
    spec->type = *format;
    format++;

    return format - start;
}

int _format_integer(char* str, size_t size, int64_t value, const struct format_spec* spec) {
    if (size == 0) return 0;

    size_t len = _itoa(value, 10, false);
    size_t total_len = len;
    char prefix[2] = {0};
    size_t prefix_len = 0;

    // Handle sign
    if (value < 0) {
        prefix[0] = '-';
        prefix_len = 1;
    } else if (spec->flags & FMT_SIGN) {
        prefix[0] = '+';
        prefix_len = 1;
    } else if (spec->flags & FMT_SPACE) {
        prefix[0] = ' ';
        prefix_len = 1;
    }

    total_len += prefix_len;

    // Calculate padding
    size_t padding = 0;
    if (spec->has_width && static_cast<size_t>(spec->width) > total_len) {
        padding = static_cast<size_t>(spec->width) - total_len;
    }

    // Write the result
    size_t written = 0;

    // Left padding
    if (!(spec->flags & FMT_LEFT) && padding > 0) {
        char pad_char = (spec->flags & FMT_ZERO) ? '0' : ' ';
        while (padding-- > 0 && written < size - 1) {
            str[written++] = pad_char;
        }
    }

    // Prefix
    if (prefix_len > 0 && written < size - 1) {
        str[written++] = prefix[0];
    }

    // Number
    for (size_t i = 0; i < len && written < size - 1; i++) {
        str[written++] = num_buffer[i];
    }

    // Right padding
    if (spec->flags & FMT_LEFT && padding > 0) {
        while (padding-- > 0 && written < size - 1) {
            str[written++] = ' ';
        }
    }

    str[written] = '\0';
    return static_cast<int>(written);
}

int _format_unsigned(char* str, size_t size, uint64_t value, const struct format_spec* spec) {
    if (size == 0) return 0;

    int base = 10;
    if (spec->type == 'x' || spec->type == 'X') {
        base = 16;
    } else if (spec->type == 'o') {
        base = 8;
    }

    size_t len = _utoa(value, base, spec->type == 'X');
    size_t total_len = len;
    char prefix[3] = {0};
    size_t prefix_len = 0;

    // Handle prefix
    if (spec->flags & FMT_ALT) {
        if (base == 16) {
            prefix[0] = '0';
            prefix[1] = spec->type == 'X' ? 'X' : 'x';
            prefix_len = 2;
        } else if (base == 8 && value != 0) {
            prefix[0] = '0';
            prefix_len = 1;
        }
    }

    total_len += prefix_len;

    // Calculate padding
    size_t padding = 0;
    if (spec->has_width && static_cast<size_t>(spec->width) > total_len) {
        padding = static_cast<size_t>(spec->width) - total_len;
    }

    // Write the result
    size_t written = 0;

    // Left padding
    if (!(spec->flags & FMT_LEFT) && padding > 0) {
        char pad_char = (spec->flags & FMT_ZERO) ? '0' : ' ';
        while (padding-- > 0 && written < size - 1) {
            str[written++] = pad_char;
        }
    }

    // Prefix
    for (size_t i = 0; i < prefix_len && written < size - 1; i++) {
        str[written++] = prefix[i];
    }

    // Number
    for (size_t i = 0; i < len && written < size - 1; i++) {
        str[written++] = num_buffer[i];
    }

    // Right padding
    if (spec->flags & FMT_LEFT && padding > 0) {
        while (padding-- > 0 && written < size - 1) {
            str[written++] = ' ';
        }
    }

    str[written] = '\0';
    return static_cast<int>(written);
}

int _format_string(char* str, size_t size, const char* value, const struct format_spec* spec) {
    if (size == 0) return 0;
    if (!value) value = "(null)";

    size_t len = strlen(value);
    if (spec->has_precision && static_cast<size_t>(spec->precision) < len) {
        len = static_cast<size_t>(spec->precision);
    }

    size_t padding = 0;
    if (spec->has_width && static_cast<size_t>(spec->width) > len) {
        padding = static_cast<size_t>(spec->width) - len;
    }

    size_t written = 0;

    // Left padding
    if (!(spec->flags & FMT_LEFT) && padding > 0) {
        while (padding-- > 0 && written < size - 1) {
            str[written++] = ' ';
        }
    }

    // String
    for (size_t i = 0; i < len && written < size - 1; i++) {
        str[written++] = value[i];
    }

    // Right padding
    if (spec->flags & FMT_LEFT && padding > 0) {
        while (padding-- > 0 && written < size - 1) {
            str[written++] = ' ';
        }
    }

    str[written] = '\0';
    return static_cast<int>(written);
}

int _format_char(char* str, size_t size, char value, const struct format_spec* spec) {
    if (size == 0) return 0;

    size_t padding = 0;
    if (spec->has_width && spec->width > 1) {
        padding = spec->width - 1;
    }

    size_t written = 0;

    // Left padding
    if (!(spec->flags & FMT_LEFT) && padding > 0) {
        while (padding-- > 0 && written < size - 1) {
            str[written++] = ' ';
        }
    }

    // Character
    if (written < size - 1) {
        str[written++] = value;
    }

    // Right padding
    if (spec->flags & FMT_LEFT && padding > 0) {
        while (padding-- > 0 && written < size - 1) {
            str[written++] = ' ';
        }
    }

    str[written] = '\0';
    return static_cast<int>(written);
}

int printf(const char* format, ...) {
    if (!format) return 0;

    va_list args;
    va_start(args, format);

    // Use a reasonable buffer size for most printf calls
    char buffer[1024];
    int result = vsnprintf(buffer, sizeof(buffer), format, args);

    // Write syscall
    if (result > 0) {
        syscall(SYS_WRITE, 0, reinterpret_cast<uint64_t>(buffer), result, 0, 0);
    }

    va_end(args);
    return result;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    if (!str || !format || size == 0) return 0;

    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

int sprintf(char* str, const char* format, ...) {
    if (!str || !format) return 0;

    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, SIZE_MAX, format, args);
    va_end(args);
    return result;
}

// Add vsnprintf implementation
int vsnprintf(char* str, size_t size, const char* format, va_list args) {
    if (!str || !format || size == 0) return 0;

    size_t written = 0;
    const char* p = format;
    struct format_spec spec;

    while (*p) {
        if (*p != '%') {
            if (written < size - 1) {
                str[written++] = *p;
            }
            p++;
            continue;
        }

        int consumed = _parse_format_spec(p, &spec);
        p += consumed;

        int result = 0;
        switch (spec.type) {
            case 'p': {
                void* value = va_arg(args, void*);
                spec.flags |= FMT_ALT;  // Force '0x' prefix
                spec.type = 'x';        // Force hex formatting
                result = _format_unsigned(str + written, size - written, reinterpret_cast<uint64_t>(value), &spec);
                break;
            }
            case 'd':
            case 'i': {
                if (spec.is_long_long) {
                    int64_t value = va_arg(args, int64_t);
                    result = _format_integer(str + written, size - written, value, &spec);
                } else {
                    int value = va_arg(args, int);
                    result = _format_integer(str + written, size - written, value, &spec);
                }
                break;
            }
            case 'u': {
                if (spec.is_long_long) {
                    uint64_t value = va_arg(args, uint64_t);
                    result = _format_unsigned(str + written, size - written, value, &spec);
                } else {
                    unsigned int value = va_arg(args, unsigned int);
                    result = _format_unsigned(str + written, size - written, value, &spec);
                }
                break;
            }
            case 'x':
            case 'X': {
                if (spec.is_long_long) {
                    uint64_t value = va_arg(args, uint64_t);
                    result = _format_unsigned(str + written, size - written, value, &spec);
                } else {
                    unsigned int value = va_arg(args, unsigned int);
                    result = _format_unsigned(str + written, size - written, value, &spec);
                }
                break;
            }
            case 'o': {
                unsigned int value = va_arg(args, unsigned int);
                result = _format_unsigned(str + written, size - written, value, &spec);
                break;
            }
            case 's': {
                const char* value = va_arg(args, const char*);
                result = _format_string(str + written, size - written, value, &spec);
                break;
            }
            case 'c': {
                char value = static_cast<char>(va_arg(args, int));
                result = _format_char(str + written, size - written, value, &spec);
                break;
            }
            case '%': {
                if (written < size - 1) {
                    str[written++] = '%';
                    result = 1;
                }
                break;
            }
            default:
                // Unknown format specifier, copy as is
                if (written < size - 1) {
                    str[written++] = '%';
                    result = 1;
                }
                p--; // Back up to reprocess the unknown specifier
                break;
        }

        written += result;
    }

    str[written] = '\0';
    return static_cast<int>(written);
}

} // extern "C"
 