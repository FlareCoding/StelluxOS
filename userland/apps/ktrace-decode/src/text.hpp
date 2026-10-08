#ifndef KTRACE_DECODE_TEXT_HPP
#define KTRACE_DECODE_TEXT_HPP

#include <cstdint>
#include <string>

std::string format_string(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Writes `count` with the noun that agrees with it, as in "1 record" or "5 records"
std::string count_with_noun(uint64_t count, const char* singular, const char* plural);

#endif // KTRACE_DECODE_TEXT_HPP
