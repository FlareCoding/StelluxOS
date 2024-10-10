#include "kstring.h"
#include <memory/kmemory.h>
#include <stdarg.h>

const char g_hexAlphabet[17] = "0123456789abcdef";

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

int htoa(
    uint64_t val,
    char* buffer,
    uint64_t bufsize
) {
    if (bufsize < 17) {
        return -1;
    }

    uint8_t idx = 0;
    while (idx < 8) {
        uint8_t* ptr = ((uint8_t*)&val) + idx;
        uint8_t currentByte = *ptr;

        buffer[15 - (idx * 2 + 1)] = g_hexAlphabet[(currentByte & 0xF0) >> 4];
        buffer[15 - (idx * 2 + 0)] = g_hexAlphabet[(currentByte & 0x0F) >> 0];

        ++idx;
    }

    buffer[idx * 2] = 0;
    return 0;
}

uint64_t strlen(const char *str) {
    const char *s = str;

    // Process bytes until aligned to 8 bytes
    while ((uint64_t)s % 8 != 0) {
        if (*s == '\0') {
            return s - str;
        }
        s++;
    }

    // Use 64-bit integers to process 8 bytes at a time
    const uint64_t *w = (const uint64_t *)s;
    while (1) {
        uint64_t v = *w;

        // Test if any of the bytes is zero
        if ((v - 0x0101010101010101) & ~v & 0x8080808080808080) {
            // Find the exact position of the null-terminator
            const char *p = (const char *)(w);
            for (int i = 0; i < 8; i++) {
                if (p[i] == '\0') {
                    return (p - str) + i;
                }
            }
        }
        w++;
    }

    // This line should never be reached.
    return 0;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

void convertUnicodeToNarrowString(void* unicodeString, char* buffer) {
    uint16_t* unicode = reinterpret_cast<uint16_t*>(unicodeString);

    if (!unicodeString || !buffer) {
        return;
    }

    // Iterate through the unicodeString until the null terminator is reached
    while (*unicode) {
        // Get the current unicode character
        uint16_t unicodeChar = *unicode;

        // If the character is in the ASCII range, directly convert it
        if (unicodeChar <= 0x7F) {
            *buffer = static_cast<char>(unicodeChar);
        } else {
            // For characters outside the ASCII range, use a placeholder
            *buffer = ' ';
        }

        // Move to the next character in both strings
        ++unicode;
        ++buffer;
    }

    // Null-terminate the output buffer
    *buffer = '\0';
}

void _vaformatStringCopy(char* dest, size_t maxLen, const char* fmt, va_list args) {
    bool fmtDiscovered = false;
    size_t index = 0;  // Index for the destination buffer

    // Helper function to append to the destination buffer
    auto appendToDest = [&](const char* str) {
        while (*str && index < maxLen - 1) {
            dest[index++] = *str++;
        }
        dest[index] = '\0';  // Null-terminate the string
    };

    // Helper function for copying a single character
    auto appendCharToDest = [&](char ch) {
        if (index < maxLen - 1) {
            dest[index++] = ch;
            dest[index] = '\0';
        }
    };

    while (*fmt && index < maxLen - 1) {
        // If the '%' character is discovered, treat it as a start of an arg format.
        if (*fmt == '%') {
            fmtDiscovered = true;
            ++fmt;
            continue;
        }

        // If no format specifier is expected, copy the character normally.
        if (!fmtDiscovered) {
            appendCharToDest(*fmt);
            ++fmt;
            continue;
        }

        // Handle format specifiers
        switch (*fmt) {
        case 'c': {
            char ch = (char)va_arg(args, int);
            appendCharToDest(ch);
            break;
        }
        case 's': {
            char* substr = va_arg(args, char*);
            appendToDest(substr);
            break;
        }
        case 'i': {
            int arg = va_arg(args, int);
            char buf[128] = { 0 };
            lltoa(arg, buf, sizeof(buf));  // Custom integer to string conversion
            appendToDest(buf);
            break;
        }
        case 'x': {
            uint32_t arg = va_arg(args, uint32_t);
            char buf[17] = { 0 };
            htoa(arg, buf, sizeof(buf));  // Custom hex conversion function
            appendToDest(buf);
            break;
        }
        case 'l': {
            char buf[128] = { 0 };

            // Handle multi-character format specifiers
            if (*(fmt + 1) == 'l' && *(fmt + 2) == 'u') {
                uint64_t arg = va_arg(args, uint64_t);
                lltoa(arg, buf, sizeof(buf));  // Custom unsigned 64-bit conversion
                fmt += 2;
            } 
            else if (*(fmt + 1) == 'l' && *(fmt + 2) == 'i') {
                int64_t arg = va_arg(args, int64_t);
                lltoa(arg, buf, sizeof(buf));  // Custom signed 64-bit conversion
                fmt += 2;
            } 
            else if (*(fmt + 1) == 'l' && *(fmt + 2) == 'x') {
                uint64_t arg = va_arg(args, uint64_t);
                htoa(arg, buf, sizeof(buf));  // Custom hex conversion for 64-bit
                fmt += 2;
            } 
            else {
                int32_t arg = va_arg(args, int32_t);
                lltoa(arg, buf, sizeof(buf));  // Default to 32-bit signed
            }

            appendToDest(buf);
            break;
        }
        default: {
            // If format specifier is unknown, treat it as a normal character
            appendCharToDest(*fmt);
            break;
        };
        }

        fmtDiscovered = false;
        ++fmt;
    }
}

void formatStringCopy(char* dest, size_t maxLen, const char* fmt, ...) {
    va_list args;
    
    // Initialize the va_list with the variable arguments
    va_start(args, fmt);
    
    // Pass the va_list to the actual formatStringCopy function
    _vaformatStringCopy(dest, maxLen, fmt, args);
    
    // Clean up the va_list
    va_end(args);
}

namespace kstl {
    string::string() : m_isUsingSSOBuffer(true) {
        m_ssoBuffer[0] = '\0';
    }

    string::~string() {
        if (!m_isUsingSSOBuffer) {
            kfree(m_data);
        }
    }

    string::string(const char* str) {
        size_t len = strlen(str); // Ensure strlen gives the correct length

        if (len < SSO_SIZE) {
            // Small string case
            memcpy(m_ssoBuffer, str, len + 1); // +1 to include null terminator
            m_isUsingSSOBuffer = true;
        } else {
            // Large string case
            m_data = static_cast<char*>(kmalloc(len + 1)); // +1 for null terminator
            memcpy(m_data, str, len + 1); // Copying the entire string including null terminator
            m_size = len;
            m_capacity = len + 1;
            m_isUsingSSOBuffer = false;
        }
    }

    string::string(const string& other) {
        size_t len = other.length();

        if (len < SSO_SIZE) {
            // Copying a small string (SSO)
            for (size_t i = 0; i <= len; ++i) {
                m_ssoBuffer[i] = other.m_ssoBuffer[i];
            }

            m_isUsingSSOBuffer = true;
        } else {
            // Copying a large string
            m_data = static_cast<char*>(kmalloc(len + 1));
            for (size_t i = 0; i <= len; ++i) {
                m_data[i] = other.m_data[i];
            }

            m_size = len;
            m_capacity = len + 1;
            m_isUsingSSOBuffer = false;
        }
    }

    string::string(string&& other) {
        // Check if the other string is using SSO buffer
        if (other.m_isUsingSSOBuffer) {
            // Copy the SSO buffer manually
            for (size_t i = 0; i <= SSO_SIZE; ++i) {
                m_ssoBuffer[i] = other.m_ssoBuffer[i];
            }

            m_isUsingSSOBuffer = true;
        } else {
            // Transfer ownership of resources
            m_data = other.m_data;
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            m_isUsingSSOBuffer = false;

            // Leave the other object's dynamic resources in a valid state
            other.m_data = nullptr;
            other.m_size = 0;
            other.m_capacity = 0;
        }

        // Null-terminate the SSO buffer of the other string
        other.m_ssoBuffer[0] = '\0';
        other.m_isUsingSSOBuffer = true;
    }

    string& string::operator=(const string& other) {
        if (this == &other) {
            // Self-assignment check
            return *this;
        }

        size_t len = other.m_size;

        if (!m_isUsingSSOBuffer) {
            // Release any existing heap memory
            kfree(m_data);
        }

        if (len < SSO_SIZE) {
            // Copying a small string (SSO)
            for (size_t i = 0; i <= len; ++i) {
                m_ssoBuffer[i] = other.m_ssoBuffer[i];
            }

            m_isUsingSSOBuffer = true;
        } else {
            // Copying a large string
            m_data = static_cast<char*>(kmalloc(len + 1));
            for (size_t i = 0; i <= len; ++i) {
                m_data[i] = other.m_data[i];
            }
            
            m_size = len;
            m_capacity = len + 1;
            m_isUsingSSOBuffer = false;
        }

        return *this;
    }

    string string::operator+(const string& other) const {
        // Create a new string with enough space to hold both strings
        string newString;
        size_t totalLength = this->length() + other.length();

        if (totalLength < SSO_SIZE) {
            // Concatenate within SSO buffer
            memcpy(newString.m_ssoBuffer, this->m_ssoBuffer, this->length());
            memcpy(newString.m_ssoBuffer + this->length(), other.m_ssoBuffer, other.length() + 1); // +1 for null terminator
            newString.m_isUsingSSOBuffer = true;
        } else {
            // Concatenate in heap-allocated memory
            newString.reserve(totalLength + 1); // +1 for null terminator
            memcpy(newString.m_data, this->c_str(), this->length());
            memcpy(newString.m_data + this->length(), other.c_str(), other.length() + 1); // +1 for null terminator
            newString.m_size = totalLength;
            newString.m_isUsingSSOBuffer = false;
        }

        return newString;
    }

    string& string::operator+=(const string& other) {
        append(other.c_str());
        return *this;
    }

    char& string::operator[](size_t index){
        return m_isUsingSSOBuffer ? m_ssoBuffer[index] : m_data[index];
    }

    const char& string::operator[](size_t index) const {
        return m_isUsingSSOBuffer ? m_ssoBuffer[index] : m_data[index];
    }

    bool string::operator==(const string& other) const {
        if (m_isUsingSSOBuffer != other.m_isUsingSSOBuffer) {
            // If one is using SSO and the other isn't, they can't be equal
            return false;
        }

        if (m_isUsingSSOBuffer) {
            // Compare SSO strings
            for (size_t i = 0; i <= SSO_SIZE; ++i) {
                if (m_ssoBuffer[i] != other.m_ssoBuffer[i]) {
                    return false;
                }

                if (m_ssoBuffer[i] == '\0') {
                    break;
                }
            }
        } else {
            // Compare large strings
            if (m_size != other.m_size) {
                return false;
            }

            for (size_t i = 0; i < m_size; ++i) {
                if (m_data[i] != other.m_data[i]) {
                    return false;
                }
            }
        }

        return true;
    }

    bool string::operator!=(const string& other) const {
        return !(*this == other);
    }

    size_t string::length() const {
        return m_isUsingSSOBuffer ? strlen(m_ssoBuffer) : m_size;
    }
    
    size_t string::capacity() const {
        return m_isUsingSSOBuffer ? SSO_SIZE : m_capacity;
    }

    void string::append(const char* str) {
        size_t len = strlen(str);
        size_t currentLength = this->length();
        size_t newLength = currentLength + len;

        if (newLength < SSO_SIZE) {
            // Append within SSO buffer
            memcpy(m_ssoBuffer + currentLength, str, len + 1); // +1 for null terminator
        } else {
            if (!m_isUsingSSOBuffer && newLength < m_capacity) {
                // Append in existing heap space
                memcpy(m_data + currentLength, str, len + 1); // +1 for null terminator
            } else {
                // Allocate new heap space
                size_t newCapacity = newLength + 1; // +1 for null terminator
                char* newData = static_cast<char*>(kmalloc(newCapacity));

                // Copy old data to new data
                memcpy(newData, m_isUsingSSOBuffer ? m_ssoBuffer : m_data, currentLength);
                memcpy(newData + currentLength, str, len + 1); // +1 for null terminator

                if (!m_isUsingSSOBuffer) {
                    // Free old heap data
                    kfree(m_data);
                }

                // Update data pointer and size/capacity
                m_data = newData;
                m_size = newLength;
                m_capacity = newCapacity;
                m_isUsingSSOBuffer = false;
            }
        }
    }

    void string::append(char chr) {
        char str[2] = {chr, '\0'};
        append(str);
    }

    void string::reserve(size_t newCapacity) {
        if (newCapacity <= capacity()) {
            // No need to reserve if the new capacity is less than or equal to the current capacity
            return;
        }

        if (m_isUsingSSOBuffer) {
            // Transition from SSO to heap allocation
            char* newData = static_cast<char*>(kmalloc(newCapacity));
            memcpy(newData, m_ssoBuffer, strlen(m_ssoBuffer) + 1); // +1 for null terminator
            m_data = newData;
        } else {
            // Reallocate more memory on the heap
            m_data = static_cast<char*>(krealloc(m_data, newCapacity));
        }

        m_capacity = newCapacity;
        m_isUsingSSOBuffer = false; // Now using heap memory
    }

    void string::resize(size_t newSize) {
        if (newSize < length()) {
            // If new size is smaller, just add a null terminator at the new size
            if (m_isUsingSSOBuffer) {
                m_ssoBuffer[newSize] = '\0';
            } else {
                m_data[newSize] = '\0';
                m_size = newSize;
            }
        } else if (newSize > length()) {
            // If new size is larger, reserve more space and set new characters to zero
            reserve(newSize);
            if (m_isUsingSSOBuffer) {
                memset(m_ssoBuffer + length(), 0, newSize - length());
            } else {
                memset(m_data + m_size, 0, newSize - m_size);
                m_size = newSize;
            }
        }
    }

    size_t string::find(char c) const {
        const char* str = m_isUsingSSOBuffer ? m_ssoBuffer : m_data;
        for (size_t i = 0; i < length(); ++i) {
            if (str[i] == c) {
                return i; // Found the character
            }
        }

        return npos;
    }

    size_t string::find(const char* str) const {
        size_t len = strlen(str);
        if (len == 0) {
            return 0; // Empty string always matches at the beginning
        }

        const char* current = m_isUsingSSOBuffer ? m_ssoBuffer : m_data;
        size_t thisLen = this->length();

        for (size_t i = 0; i <= thisLen - len; ++i) {
            size_t j;
            for (j = 0; j < len; ++j) {
                if (current[i + j] != str[j]) {
                    break; // Mismatch found, break inner loop
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

    string string::substring(size_t start, size_t length) const {
        size_t strLength = this->length();

        if (start >= strLength) {
            return string(); // Return an empty string if start is out of bounds
        }

        // If length is npos or extends beyond the end of the string,
        // adjust it to go to the end of the string
        if (length == npos || start + length > strLength) {
            length = strLength - start;
        }

        char* buffer = static_cast<char*>(kmalloc(length + 1)); // +1 for null terminator

        const char* str = m_isUsingSSOBuffer ? m_ssoBuffer : m_data;
        
        // Copy the substring into the buffer
        memcpy(buffer, str + start, length);
        buffer[length] = '\0'; // Null-terminate the buffer

        string newString(buffer);
        kfree(buffer); // Free the temporary buffer

        return newString;
    }

    void string::clear() {
        if (!m_isUsingSSOBuffer && m_capacity) {
            kfree(m_data); // Free heap memory if used
        }

        // Reset to an empty SSO string
        m_ssoBuffer[0] = '\0';
        m_isUsingSSOBuffer = true;
    }

    const char* string::data() const {
        return c_str();
    }

    const char* string::c_str() const {
        return m_isUsingSSOBuffer ? m_ssoBuffer : m_data;
    }

    // Convert integer to string
    string to_string(int value) {
        char buf[32] = { 0 };  // Large enough to hold any int
        lltoa(value, buf, sizeof(buf));  // Use your custom integer-to-string conversion
        return string(buf);
    }

    // Convert unsigned integer to string
    string to_string(unsigned int value) {
        char buf[32] = { 0 };  // Large enough to hold any unsigned int
        lltoa(value, buf, sizeof(buf));  // Use your custom integer-to-string conversion
        return string(buf);
    }
} // namespace kstl
