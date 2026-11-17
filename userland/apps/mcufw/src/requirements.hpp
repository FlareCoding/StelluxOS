#ifndef MCUFW_REQUIREMENTS_HPP
#define MCUFW_REQUIREMENTS_HPP

// What a board's build is missing: the core at a version the image has overrides for, each tool its recipes name that
// is not installed or that the core does not depend on, and the program the compile recipe runs when it is not found

#include "boards.hpp"
#include "tools.hpp"

#include <string>
#include <vector>

struct board_requirements {
    std::vector<tool_dependency> tools;              // the core's dependencies the recipes name, located or not
    std::vector<tool_dependency> missing_tools;      // uninstalled dependencies, and names the core does not depend on
    std::string                  missing_program;    // the compile recipe's program, by file name, when it is not found
    std::string                  missing_overrides;  // as `esp32:esp32 3.3.12`, its newest overridden version
};

// Each tool named once. The program is checked only once its path holds no placeholder, so a missing tool hides the
// program it would hold, and a compile recipe that is unset or does not split names no program.
board_requirements check_requirements(const selected_board& board, const std::vector<located_tool>& tools);

// What is missing as `mcufw boards` writes it, such as `xtensa-esp32s3-elf-g++, esp32:esp32s3-libs 3.3.12`, or empty
// when nothing is
std::string describe_missing(const board_requirements& requirements);

#endif // MCUFW_REQUIREMENTS_HPP
