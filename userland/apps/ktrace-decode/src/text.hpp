#ifndef KTRACE_DECODE_TEXT_HPP
#define KTRACE_DECODE_TEXT_HPP

#include <string>

std::string format_string(const char* format, ...) __attribute__((format(printf, 1, 2)));

#endif // KTRACE_DECODE_TEXT_HPP
