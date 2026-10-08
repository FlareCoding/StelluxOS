#include "text.hpp"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>

std::string format_string(const char* format, ...) {
    va_list args;
    va_start(args, format);

    va_list measured_args;
    va_copy(measured_args, args);
    int length = vsnprintf(nullptr, 0, format, measured_args);
    va_end(measured_args);

    std::string text(length > 0 ? static_cast<size_t>(length) : 0, '\0');
    vsnprintf(text.data(), text.size() + 1, format, args);
    va_end(args);

    return text;
}

std::string count_with_noun(uint64_t count, const char* singular, const char* plural) {
    return format_string("%" PRIu64 " %s", count, count == 1 ? singular : plural);
}
