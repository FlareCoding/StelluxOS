#ifndef KTRACE_DECODE_HEALTH_HPP
#define KTRACE_DECODE_HEALTH_HPP

#include "trace_file.hpp"

#include <string>
#include <vector>

struct health_check {
    const char* name;
    bool        passed;
    std::string detail;
};

// Checks whether the file holds the whole session as it was recorded
std::vector<health_check> check_health(const trace_file& file, const record_counts& counts);

#endif // KTRACE_DECODE_HEALTH_HPP
