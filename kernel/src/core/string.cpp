#include <string.h>
#include <memory/memory.h>

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

char* strncat(char* dest, const char* src, size_t n) {
    char* original_dest = dest;

    // Find the end of the destination string
    while (*dest) {
        dest++;
    }

    // Append up to `n` characters from `src` to `dest`
    while (n-- > 0 && *src) {
        *dest++ = *src++;
    }

    // Null-terminate the resulting string
    *dest = '\0';

    return original_dest;
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

            // Parse flags
            int width = 0;
            char padding_char = ' ';  // Default padding is space
            if (*ptr == '0') {
                padding_char = '0';
                ptr++;
            }

            // Parse width
            while (*ptr >= '0' && *ptr <= '9') {
                width = width * 10 + (*ptr - '0');
                ptr++;
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
                int padding = (width > num_len) ? width - num_len : 0;
                while (padding-- > 0 && buffer_index < buffer_size - 1) {
                    buffer[buffer_index++] = padding_char;
                }
                for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                    buffer[buffer_index++] = num_buffer[i];
                }
            }
            else if (*ptr == 'c') {
                // %c - single character
                char char_arg = (char)va_arg(args, int); // Characters are promoted to int in variadic arguments
                buffer[buffer_index++] = char_arg;
            }
            else if (*ptr == 'u') {
                // %u - uint32_t
                uint32_t uint_arg = va_arg(args, uint32_t);
                char num_buffer[11]; // Enough for 32-bit unsigned int
                int num_len = uint_to_str((uint64_t)uint_arg, num_buffer, sizeof(num_buffer), 10);
                int padding = (width > num_len) ? width - num_len : 0;
                while (padding-- > 0 && buffer_index < buffer_size - 1) {
                    buffer[buffer_index++] = padding_char;
                }
                for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                    buffer[buffer_index++] = num_buffer[i];
                }
            }
            else if (*ptr == 'l') {
                // Handle long specifiers: %lli, %llu, %llx, %016llx
                ptr++;
                if (*ptr == 'l') {
                    ptr++;
                    if (*ptr == 'i') {
                        // %lli - int64_t
                        int64_t long_long_int_arg = va_arg(args, int64_t);
                        char num_buffer[21]; // Enough for 64-bit int
                        int num_len = int_to_str(long_long_int_arg, num_buffer, sizeof(num_buffer), 10);
                        int padding = (width > num_len) ? width - num_len : 0;
                        while (padding-- > 0 && buffer_index < buffer_size - 1) {
                            buffer[buffer_index++] = padding_char;
                        }
                        for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                            buffer[buffer_index++] = num_buffer[i];
                        }
                    }
                    else if (*ptr == 'u') {
                        // %llu - uint64_t
                        uint64_t long_long_uint_arg = va_arg(args, uint64_t);
                        char num_buffer[21]; // Enough for 64-bit unsigned int
                        int num_len = uint_to_str(long_long_uint_arg, num_buffer, sizeof(num_buffer), 10);
                        int padding = (width > num_len) ? width - num_len : 0;
                        while (padding-- > 0 && buffer_index < buffer_size - 1) {
                            buffer[buffer_index++] = padding_char;
                        }
                        for (int i = 0; i < num_len && buffer_index < buffer_size - 1; i++) {
                            buffer[buffer_index++] = num_buffer[i];
                        }
                    }
                    else if (*ptr == 'x') {
                        // %llx or %016llx - uint64_t in hexadecimal
                        uint64_t long_long_hex_arg = va_arg(args, uint64_t);
                        char num_buffer[17]; // 16 hex digits + null terminator for 64-bit
                        int num_len = uint_to_str(long_long_hex_arg, num_buffer, sizeof(num_buffer), 16);
                        int padding = (width > num_len) ? width - num_len : 0;
                        while (padding-- > 0 && buffer_index < buffer_size - 1) {
                            buffer[buffer_index++] = padding_char;
                        }
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
                // %x or %016x - uint32_t in hexadecimal
                uint32_t hex_arg = va_arg(args, uint32_t);
                char num_buffer[9]; // 8 hex digits + null terminator for 32-bit
                int num_len = uint_to_str((uint64_t)hex_arg, num_buffer, sizeof(num_buffer), 16);
                int padding = (width > num_len) ? width - num_len : 0;
                while (padding-- > 0 && buffer_index < buffer_size - 1) {
                    buffer[buffer_index++] = padding_char;
                }
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

void convert_unicode_to_narrow_string(void* unicode_string, char* buffer) {
    if (unicode_string == nullptr || buffer == nullptr) {
        // Handle null pointers appropriately (could log an error or handle as needed)
        return;
    }

    uint16_t* u_str = static_cast<uint16_t*>(unicode_string);
    size_t i = 0; // Index for the Unicode string
    size_t j = 0; // Index for the narrow string

    while (u_str[i] != 0) { // Continue until null-terminator is reached
        uint16_t code_unit = u_str[i];

        if (code_unit < 128) { // ASCII range
            buffer[j++] = static_cast<char>(code_unit);
        } else {
            buffer[j++] = '?'; // Placeholder for non-ASCII characters
        }

        i++;
    }

    buffer[j] = '\0'; // Null-terminate the resulting string
}

int lltoa(
    uint64_t val,
    char* buffer,
    uint64_t bufsize
) {
    uint8_t length = 0;
    uint64_t lenTest = val;
    while (lenTest / 10 > 0) {
        lenTest /= 10;
        ++length;
    }

    if (bufsize < static_cast<uint64_t>(length + 1)) {
        return -1;
    }

    uint8_t index = 0;
    while (val / 10 > 0) {
        uint8_t remainder = val % 10;
        val /= 10;

        buffer[length - index] = remainder + '0';
        ++index;
    }

    // Last digit
    uint8_t remainder = val % 10;
    buffer[length - index] = remainder + '0';

    // Add the null-terminator
    buffer[length + 1] = 0;

    return 0;
}

int itoa(
    int32_t val,
    char* buffer,
    uint64_t bufsize
) {
    bool negative = false;
    if (val < 0) {
        negative = true;
        val *= -1;
        buffer[0] = '-';
    }

    uint8_t length = 0;
    uint64_t lenTest = val;
    while (lenTest / 10 > 0) {
        lenTest /= 10;
        ++length;
    }

    if (bufsize < static_cast<uint64_t>(negative + length + 1)) {
        return -1;
    }

    uint8_t index = 0;
    while (val / 10 > 0) {
        uint8_t remainder = val % 10;
        val /= 10;

        buffer[negative + length - index] = remainder + '0';
        ++index;
    }

    // Last digit
    uint8_t remainder = val % 10;
    buffer[negative + length - index] = remainder + '0';

    // Add the null-terminator
    buffer[negative + length + 1] = 0;

    return 0;
}

namespace kstl {
string::string()
    : m_data(nullptr),
      m_size(0),
      m_capacity(0),
      m_using_sso(true) {
    m_sso_buffer[0] = '\0';
}

string::~string() {
    if (!m_using_sso) {
        free(m_data);
    }
}

string::string(const char* str) {
    size_t len = strlen(str);

    if (len < m_sso_size) {
        // Small string case
        memcpy(m_sso_buffer, str, len + 1); // +1 to include null terminator
        m_using_sso = true;
        m_size = len;
        m_capacity = m_sso_size;
    } else {
        // Large string case
        m_data = static_cast<char*>(malloc(len + 1)); // +1 for null terminator
        memcpy(m_data, str, len + 1);
        m_size = len;
        m_capacity = len + 1;
        m_using_sso = false;
    }
}

string::string(const string& other) {
    size_t len = other.length();

    if (len < m_sso_size) {
        // Copying a small string (SSO)
        for (size_t i = 0; i <= len; ++i) {
            m_sso_buffer[i] = other.m_sso_buffer[i];
        }

        m_using_sso = true;
        m_size = len;
        m_capacity = m_sso_size;
    } else {
        // Copying a large string
        m_data = static_cast<char*>(malloc(len + 1));
        for (size_t i = 0; i <= len; ++i) {
            m_data[i] = other.m_data[i];
        }

        m_size = len;
        m_capacity = len + 1;
        m_using_sso = false;
    }
}

string::string(string&& other) {
    if (other.m_using_sso) {
        // If the other string is using SSO, just copy its SSO buffer
        // and mark other as empty.
        m_size = other.m_size;
        m_capacity = other.m_capacity; // This might be equal to m_sso_size
        m_using_sso = true;
        // Copy the SSO buffer including the null terminator
        for (size_t i = 0; i <= m_size; ++i) {
            m_sso_buffer[i] = other.m_sso_buffer[i];
        }
        m_data = nullptr; // We do not use m_data in SSO

        // Reset the other string to empty state
        other.m_sso_buffer[0] = '\0';
        other.m_size = 0;
        other.m_capacity = m_sso_size;
        other.m_using_sso = true;
        other.m_data = nullptr;
    } else {
        // If the other string was using a dynamic buffer
        // Just steal the pointer and data
        m_data = other.m_data;
        m_size = other.m_size;
        m_capacity = other.m_capacity;
        m_using_sso = false;
        // The SSO buffer isn't used since we're in heap mode, but let's
        // keep it clean:
        m_sso_buffer[0] = '\0';

        // Now reset the other string to an empty SSO-based string
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_sso_buffer[0] = '\0';
        other.m_capacity = m_sso_size;
        other.m_using_sso = true;
    }
}

string& string::operator=(const string& other) {
    if (this == &other) {
        // Self-assignment check
        return *this;
    }

    size_t len = other.length();

    if (!m_using_sso) {
        // Release any existing heap memory
        free(m_data);
    }

    if (len < m_sso_size) {
        // Copying a small string (SSO)
        for (size_t i = 0; i <= len; ++i) {
            m_sso_buffer[i] = other.m_sso_buffer[i];
        }

        m_using_sso = true;
        m_size = len;
        m_capacity = m_sso_size;
    } else {
        // Copying a large string
        m_data = static_cast<char*>(malloc(len + 1));
        for (size_t i = 0; i <= len; ++i) {
            m_data[i] = other.m_data[i];
        }

        m_size = len;
        m_capacity = len + 1;
        m_using_sso = false;
    }

    return *this;
}

string string::operator+(const string& other) const {
    // Create a new string with enough space to hold both strings
    size_t total_length = this->length() + other.length();
    string new_string;

    if (total_length < m_sso_size) {
        // Concatenate within SSO buffer
        memcpy(new_string.m_sso_buffer, this->c_str(), this->length());
        memcpy(new_string.m_sso_buffer + this->length(), other.c_str(), other.length() + 1); // +1 for null terminator
        new_string.m_using_sso = true;
        new_string.m_size = total_length;
        new_string.m_capacity = m_sso_size;
    } else {
        // Concatenate in heap-allocated memory
        new_string.reserve(total_length + 1); // +1 for null terminator
        memcpy(new_string.m_data, this->c_str(), this->length());
        memcpy(new_string.m_data + this->length(), other.c_str(), other.length() + 1); // +1 for null terminator
        new_string.m_size = total_length;
        new_string.m_using_sso = false;
    }

    return new_string;
}

string& string::operator+=(const string& other) {
    append(other.c_str());
    return *this;
}

char& string::operator[](size_t index) {
    return m_using_sso ? m_sso_buffer[index] : m_data[index];
}

const char& string::operator[](size_t index) const {
    return m_using_sso ? m_sso_buffer[index] : m_data[index];
}

bool string::operator==(const string& other) const {
    // First check if both strings have the same length
    if (this->length() != other.length()) {
        return false;
    }

    if (m_using_sso && other.m_using_sso) {
        // Compare SSO strings
        for (size_t i = 0; i <= m_sso_size; ++i) {
            if (m_sso_buffer[i] != other.m_sso_buffer[i]) {
                return false;
            }
            if (m_sso_buffer[i] == '\0') {
                break;
            }
        }
        return true;
    } else if (!m_using_sso && !other.m_using_sso) {
        // Compare large strings
        for (size_t i = 0; i < m_size; ++i) {
            if (m_data[i] != other.m_data[i]) {
                return false;
            }
        }
        return true;
    } else {
        // One uses SSO, the other does not, but lengths matched. Check actual chars.
        const char* this_str = c_str();
        const char* other_str = other.c_str();
        for (size_t i = 0; i < this->length(); ++i) {
            if (this_str[i] != other_str[i]) {
                return false;
            }
        }
        return true;
    }
}

bool string::operator!=(const string& other) const {
    return !(*this == other);
}

size_t string::length() const {
    return m_size;
}

size_t string::capacity() const {
    return m_using_sso ? m_sso_size : m_capacity;
}

void string::append(const char* str) {
    size_t len = strlen(str);
    size_t current_length = this->length();
    size_t new_length = current_length + len;

    if (new_length < m_sso_size) {
        // Append within SSO buffer
        memcpy(m_sso_buffer + current_length, str, len + 1); // +1 for null terminator
        m_using_sso = true; // still SSO
        m_size = new_length;
    } else {
        if (!m_using_sso && new_length < m_capacity) {
            // Append in existing heap space
            memcpy(m_data + current_length, str, len + 1); // +1 for null terminator
            m_size = new_length;
        } else {
            // Allocate new heap space
            size_t new_capacity = new_length + 1; // +1 for null terminator
            char* new_data = static_cast<char*>(malloc(new_capacity));

            // Copy old data to new data
            memcpy(new_data, m_using_sso ? m_sso_buffer : m_data, current_length);
            memcpy(new_data + current_length, str, len + 1); // +1 for null terminator

            if (!m_using_sso) {
                // Free old heap data
                free(m_data);
            }

            // Update data pointer and size/capacity
            m_data = new_data;
            m_size = new_length;
            m_capacity = new_capacity;
            m_using_sso = false;
        }
    }
}

void string::append(char chr) {
    char temp[2] = { chr, '\0' };
    append(temp);
}

void string::reserve(size_t new_capacity) {
    if (new_capacity <= capacity()) {
        return;
    }

    if (m_using_sso) {
        // Transition from SSO to heap allocation
        size_t cur_length = length();
        char* new_data = static_cast<char*>(malloc(new_capacity));
        memcpy(new_data, m_sso_buffer, cur_length + 1); // include null terminator
        m_data = new_data;
        m_size = cur_length;
    } else {
        // Reallocate more memory on the heap
        // Assuming krealloc is available and works similarly to realloc
        m_data = static_cast<char*>(realloc(m_data, new_capacity));
    }

    m_capacity = new_capacity;
    m_using_sso = false; // Now using heap memory
}

void string::resize(size_t new_size) {
    if (new_size < m_size) {
        // Shrinking
        m_size = new_size;
        if (m_using_sso) {
            m_sso_buffer[m_size] = '\0';
        } else {
            m_data[m_size] = '\0';
        }
    } else if (new_size > m_size) {
        // Growing
        if (new_size > m_capacity) {
            reserve(new_size);
        }

        // Fill with '\0' from old size to new_size
        if (m_using_sso) {
            for (size_t i = m_size; i < new_size; ++i) {
                m_sso_buffer[i] = '\0';
            }
            m_sso_buffer[new_size] = '\0';
        } else {
            for (size_t i = m_size; i < new_size; ++i) {
                m_data[i] = '\0';
            }
            m_data[new_size] = '\0';
        }

        m_size = new_size;
    }
}

size_t string::find(char c) const {
    const char* str = m_using_sso ? m_sso_buffer : m_data;
    for (size_t i = 0; i < length(); ++i) {
        if (str[i] == c) {
            return i; // Found the character
        }
    }

    return npos;
}

size_t string::find(const char* substr) const {
    size_t len = strlen(substr);
    if (len == 0) {
        return 0; // Empty string always matches at the beginning
    }

    const char* current = m_using_sso ? m_sso_buffer : m_data;
    size_t this_len = this->length();

    if (len > this_len) {
        return npos;
    }

    for (size_t i = 0; i <= this_len - len; ++i) {
        size_t j;
        for (j = 0; j < len; ++j) {
            if (current[i + j] != substr[j]) {
                break;
            }
        }

        if (j == len) {
            return i; // Substring found at position i
        }
    }

    return npos; // Substring not found
}

size_t string::find(const string& str) const {
    return find(str.c_str());
}

size_t string::find(char c, size_t start) const {
    const char* current = m_using_sso ? m_sso_buffer : m_data;
    size_t this_len = this->length();

    if (start >= this_len) {
        return npos; // Starting index out of bounds
    }

    for (size_t i = start; i < this_len; ++i) {
        if (current[i] == c) {
            return i; // Found the character
        }
    }

    return npos; // Character not found
}

size_t string::find(const char* substr, size_t start) const {
    size_t len = strlen(substr);
    if (len == 0) {
        return start; // Empty string always matches at the starting index
    }

    const char* current = m_using_sso ? m_sso_buffer : m_data;
    size_t this_len = this->length();

    if (start >= this_len || len > this_len - start) {
        return npos; // Starting index out of bounds or substring too long
    }

    for (size_t i = start; i <= this_len - len; ++i) {
        size_t j;
        for (j = 0; j < len; ++j) {
            if (current[i + j] != substr[j]) {
                break;
            }
        }

        if (j == len) {
            return i; // Substring found at position i
        }
    }

    return npos; // Substring not found
}

size_t string::find(const string& str, size_t start) const {
    return find(str.c_str(), start);
}

size_t string::find_last_of(char c) const {
    const char* str = m_using_sso ? m_sso_buffer : m_data;
    size_t len = this->length();

    // Traverse the string backward to find the last occurrence of the character
    for (size_t i = len; i > 0; --i) {
        if (str[i - 1] == c) {
            return i - 1; // Return the index of the last occurrence
        }
    }

    return npos; // Character not found
}

string string::substring(size_t start, size_t length) const {
    size_t str_length = this->length();

    if (start >= str_length) {
        return string(); // Return an empty string if start is out of bounds
    }

    // If length is npos or extends beyond the end, adjust it
    if (length == npos || start + length > str_length) {
        length = str_length - start;
    }

    char* buffer = static_cast<char*>(malloc(length + 1)); // +1 for null terminator

    const char* str = m_using_sso ? m_sso_buffer : m_data;
    
    memcpy(buffer, str + start, length);
    buffer[length] = '\0'; // Null-terminate

    string new_string(buffer);
    free(buffer); // Free temporary buffer

    return new_string;
}

bool string::starts_with(const kstl::string& prefix) const {
    size_t prefix_length = prefix.length();
    size_t str_length = this->length();

    // If the prefix is longer than the string, it cannot match
    if (prefix_length > str_length) {
        return false;
    }

    const char* str = this->c_str();
    const char* prefix_str = prefix.c_str();

    // Compare the prefix with the start of the string
    for (size_t i = 0; i < prefix_length; ++i) {
        if (str[i] != prefix_str[i]) {
            return false;
        }
    }

    return true;
}

void string::clear() {
    if (!m_using_sso && m_capacity) {
        free(m_data);
    }

    m_sso_buffer[0] = '\0';
    m_using_sso = true;
    m_size = 0;
    m_capacity = m_sso_size;
}

const char* string::data() const {
    return c_str();
}

const char* string::c_str() const {
    return m_using_sso ? m_sso_buffer : m_data;
}

// Convert integer to string
string to_string(int value) {
    char buf[32] = { 0 };  // Large enough for an int
    itoa(value, buf, sizeof(buf));  // Custom integer-to-string conversion
    return string(buf);
}

// Convert unsigned integer to string
string to_string(unsigned int value) {
    char buf[32] = { 0 };  // Large enough for unsigned int
    lltoa(value, buf, sizeof(buf));  // Custom integer-to-string conversion
    return string(buf);
}
} // namespace kstl
