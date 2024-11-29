#include <string.h>

void reverse_str(char* str, int length) {
    int start = 0;
    int end = length - 1;
    char temp;
    while (start < end) {
        temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

int strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    // Cast to unsigned char to ensure correct comparison of characters with high ASCII values
    return *(const unsigned char*)str1 - *(const unsigned char*)str2;
}

char* strcpy(char* dest, const char* src) {
    char* original_dest = dest;
    while ((*dest++ = *src++) != '\0') {
        // Copy each character including the null terminator
    }
    return original_dest;
}

size_t strlen(const char* str) {
    size_t length = 0;
    while (*str++) {
        length++;
    }
    return length;
}

int uint_to_str(uint64_t value, char* buffer, size_t buffer_size, int base) {
    const char digits[] = "0123456789abcdef";
    int i = 0;

    if (value == 0) {
        if (i < (int)(buffer_size - 1)) {
            buffer[i++] = '0';
        }
    } else {
        while (value != 0 && i < (int)(buffer_size - 1)) {
            buffer[i++] = digits[value % base];
            value /= base;
        }
    }

    buffer[i] = '\0';
    reverse_str(buffer, i);
    return i;
}

int int_to_str(int64_t value, char* buffer, size_t buffer_size, int base) {
    int i = 0;
    bool is_negative = false;
    uint64_t uvalue;

    if (value < 0 && base == 10) {
        is_negative = true;
        uvalue = -value;
    } else {
        uvalue = value;
    }

    i = uint_to_str(uvalue, buffer, buffer_size, base);

    if (is_negative) {
        if (i < (int)(buffer_size - 1)) {
            // Shift the string to the right to make space for '-'
            for (int j = i; j > 0; j--) {
                buffer[j] = buffer[j - 1];
            }
            buffer[0] = '-';
            i++;
            buffer[i] = '\0';
        }
    }

    return i;
}

int sprintf(char* buffer, size_t buffer_size, const char* format, ...) {
    va_list args;
    va_start(args, format);

    size_t buffer_index = 0;
    const char* ptr = format;

    while (*ptr && buffer_index < buffer_size - 1) {  // Reserve space for null terminator
        if (*ptr == '%') {
            ptr++;
            if (*ptr == '\0') {
                break; // Trailing '%' character
            }

            // Handle format specifiers
            if (*ptr == 's') {
                // %s - null-terminated string
                const char* str_arg = va_arg(args, const char*);
                while (*str_arg && buffer_index < buffer_size - 1) {
                    buffer[buffer_index++] = *str_arg++;
                }
            }
            else if (*ptr == 'd' || *ptr == 'i') {
                // %d or %i - int32_t
                int32_t int_arg = va_arg(args, int32_t);
                char num_buffer[12]; // Enough for 32-bit int
                int num_len = int_to_str((int64_t)int_arg, num_buffer, sizeof(num_buffer), 10);
                for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                    buffer[buffer_index++] = num_buffer[i];
                }
            }
            else if (*ptr == 'u') {
                // %u - uint32_t
                uint32_t uint_arg = va_arg(args, uint32_t);
                char num_buffer[11]; // Enough for 32-bit unsigned int
                int num_len = uint_to_str((uint64_t)uint_arg, num_buffer, sizeof(num_buffer), 10);
                for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                    buffer[buffer_index++] = num_buffer[i];
                }
            }
            else if (*ptr == 'l') {
                // Handle long specifiers: %lli, %llu, %llx
                ptr++;
                if (*ptr == 'l') {
                    ptr++;
                    if (*ptr == 'i') {
                        // %lli - int64_t
                        int64_t long_long_int_arg = va_arg(args, int64_t);
                        char num_buffer[21]; // Enough for 64-bit int
                        int num_len = int_to_str(long_long_int_arg, num_buffer, sizeof(num_buffer), 10);
                        for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                            buffer[buffer_index++] = num_buffer[i];
                        }
                    }
                    else if (*ptr == 'u') {
                        // %llu - uint64_t
                        uint64_t long_long_uint_arg = va_arg(args, uint64_t);
                        char num_buffer[21]; // Enough for 64-bit unsigned int
                        int num_len = uint_to_str(long_long_uint_arg, num_buffer, sizeof(num_buffer), 10);
                        for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                            buffer[buffer_index++] = num_buffer[i];
                        }
                    }
                    else if (*ptr == 'x') {
                        // %llx - uint64_t in hexadecimal
                        uint64_t long_long_hex_arg = va_arg(args, uint64_t);
                        char num_buffer[17]; // 16 hex digits + null terminator for 64-bit
                        int num_len = uint_to_str(long_long_hex_arg, num_buffer, sizeof(num_buffer), 16);
                        for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                            buffer[buffer_index++] = num_buffer[i];
                        }
                    }
                    else {
                        // Unsupported specifier after %ll, treat as literal
                        buffer[buffer_index++] = '%';
                        buffer[buffer_index++] = 'l';
                        buffer[buffer_index++] = 'l';
                        if (buffer_index < buffer_size - 1) {
                            buffer[buffer_index++] = *ptr;
                        }
                    }
                }
                else {
                    // Unsupported specifier after %l, treat as literal
                    buffer[buffer_index++] = '%';
                    buffer[buffer_index++] = 'l';
                    if (buffer_index < buffer_size - 1) {
                        buffer[buffer_index++] = *ptr;
                    }
                }
            }
            else if (*ptr == 'x') {
                // %x - uint32_t in hexadecimal
                uint32_t hex_arg = va_arg(args, uint32_t);
                char num_buffer[9]; // 8 hex digits + null terminator for 32-bit
                int num_len = uint_to_str((uint64_t)hex_arg, num_buffer, sizeof(num_buffer), 16);
                for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                    buffer[buffer_index++] = num_buffer[i];
                }
            }
            else if (*ptr == '%') {
                // %% - literal '%'
                buffer[buffer_index++] = '%';
            }
            else {
                // Unsupported format specifier, treat as literal
                buffer[buffer_index++] = '%';
                if (buffer_index < buffer_size - 1) {
                    buffer[buffer_index++] = *ptr;
                }
            }
        }
        else {
            buffer[buffer_index++] = *ptr;
        }
        ptr++;
    }

    buffer[buffer_index] = '\0'; // Null-terminate the buffer
    va_end(args);
    return buffer_index;
}
