#ifndef KSTRING_H
#define KSTRING_H
#include <ktypes.h>

// uint64_t --> string
int lltoa(
    uint64_t val,
    char* buffer,
    uint64_t bufsize
);

// int32_t --> string
int itoa(
    int32_t val,
    char* buffer,
    uint64_t bufsize
);

// uint64_t --> hex string
int htoa(
    uint64_t val,
    char* buffer,
    uint64_t bufsize
);

// double --> string
int ftoa(
    double val,
    char* buffer,
    uint64_t bufsize
);

uint64_t strlen(const char *str);

namespace kstl {
class string {
public:
    static const size_t npos = static_cast<size_t>(-1);

    string();
    ~string();

    string(const char* str);
    string(const string& other);
    string(string&& other);

    string& operator=(const string& other);
    char& operator[](size_t index);
    const char& operator[](size_t index) const;
    bool operator==(const string& other) const;
    bool operator!=(const string& other) const;

    size_t length() const;
    size_t capacity() const;

    void append(const char* str);
    void append(char chr);

    void reserve(size_t newCapacity);
    void resize(size_t newSize);

    size_t find(char c) const;
    size_t find(const char* str) const;
    size_t find(const string& str) const;

    string substring(size_t start, size_t length = npos) const;

    void clear();

    inline bool empty() const { return m_size > 0; }

    const char* data() const;
    const char* c_str() const;

private:
    static const size_t SSO_SIZE = 15;

    char m_ssoBuffer[SSO_SIZE + 1] = { 0 };

    struct {
        char*   m_data;
        size_t  m_size;
        size_t  m_capacity;
    };

    bool m_isUsingSSOBuffer;
};
} // namespace kstl

#endif