#ifndef MCUFW_COMPILER_OUTPUT_HPP
#define MCUFW_COMPILER_OUTPUT_HPP

// GCC's messages read for the problems they name, `<file>:<line>:<column>: <severity>: <message>`

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class problem_severity { error, warning, note };

struct compiler_problem {
    problem_severity severity;  // a fatal error reads as an error
    std::string      file;
    uint32_t         line;
    uint32_t         column;    // 0 when the message gives none
    std::string      message;
};

std::vector<compiler_problem> find_compiler_problems(std::string_view output);

#endif // MCUFW_COMPILER_OUTPUT_HPP
