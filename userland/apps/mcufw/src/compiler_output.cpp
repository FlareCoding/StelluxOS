#include "compiler_output.hpp"

#include <array>
#include <utility>

static bool read_number(std::string_view& location, uint32_t& number) {
    size_t colon = location.rfind(':');
    if (colon == std::string_view::npos || colon + 1 == location.size()) {
        return false;
    }

    uint64_t value = 0;
    for (char c : location.substr(colon + 1)) {
        if (c < '0' || c > '9' || value > UINT32_MAX / 10) {
            return false;
        }

        value = value * 10 + static_cast<uint64_t>(c - '0');
    }

    number = static_cast<uint32_t>(value);
    location = location.substr(0, colon);
    return true;
}

// `<file>:<line>:<column>` or `<file>:<line>`
static bool read_location(std::string_view location, compiler_problem& problem) {
    uint32_t last = 0;
    if (!read_number(location, last)) {
        return false;
    }

    uint32_t line = 0;
    if (read_number(location, line)) {
        problem.line = line;
        problem.column = last;
    } else {
        problem.line = last;
        problem.column = 0;
    }

    problem.file = location;
    return !problem.file.empty();
}

static bool read_problem(std::string_view line, compiler_problem& problem) {
    static constexpr std::array<std::pair<std::string_view, problem_severity>, 4> severities = { {
        { "fatal error: ", problem_severity::error },
        { "error: ", problem_severity::error },
        { "warning: ", problem_severity::warning },
        { "note: ", problem_severity::note },
    } };

    for (size_t at = line.find(": "); at != std::string_view::npos; at = line.find(": ", at + 1)) {
        std::string_view rest = line.substr(at + 2);
        for (const auto& [word, severity] : severities) {
            if (rest.substr(0, word.size()) == word && read_location(line.substr(0, at), problem)) {
                problem.severity = severity;
                problem.message = rest.substr(word.size());
                return true;
            }
        }
    }

    return false;
}

std::vector<compiler_problem> find_compiler_problems(std::string_view output) {
    std::vector<compiler_problem> problems;
    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find('\n', start);
        if (end == std::string_view::npos) {
            end = output.size();
        }

        std::string_view line = output.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        compiler_problem problem;
        if (read_problem(line, problem)) {
            problems.push_back(problem);
        }

        start = end + 1;
    }

    return problems;
}
