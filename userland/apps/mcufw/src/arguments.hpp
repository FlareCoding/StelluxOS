#ifndef MCUFW_ARGUMENTS_HPP
#define MCUFW_ARGUMENTS_HPP

// mcufw's command line: the command, its operands, and the options that command takes

#include <cstdint>
#include <string>
#include <vector>

enum class command_name { boards, install, remove, hook, build, clean, flash, monitor };

struct arguments {
    command_name             command = command_name::boards;
    std::vector<std::string> operands;
    bool                     events = false;
    std::string              board;                 // build and flash: `--board`
    uint32_t                 jobs = 0;              // build and flash: `--jobs`, 0 for one per CPU
    std::string              warnings = "default";  // build and flash: `--warnings`, `default` as decision 6 has it
    bool                     verbose = false;       // build and flash: `--verbose`, which the other commands accept
    bool                     cores = false;         // clean: `--cores`
    std::string              port;                  // flash and monitor: `--port`
    uint32_t                 baud = 115200;         // flash and monitor: `--baud`
    bool                     monitor = false;       // flash: `--monitor`
    std::vector<std::string> serial_options;        // flash and monitor: the words after `--`
};

// Fails on a missing or unknown command or a word the command does not take, which main reports as a usage error.
// `hook` takes every word after it as an operand, options included.
bool parse_arguments(int argc, char** argv, arguments& parsed, std::string& error);

#endif // MCUFW_ARGUMENTS_HPP
