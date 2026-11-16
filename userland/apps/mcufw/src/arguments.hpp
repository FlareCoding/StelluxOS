#ifndef MCUFW_ARGUMENTS_HPP
#define MCUFW_ARGUMENTS_HPP

// mcufw's command line: the command, its operands, and the options that command takes

#include <string>
#include <vector>

enum class command_name { boards };

struct arguments {
    command_name             command = command_name::boards;
    std::vector<std::string> operands;
    bool                     events = false;
};

// Fails on a missing or unknown command or a word the command does not take, which main reports as a usage error
bool parse_arguments(int argc, char** argv, arguments& parsed, std::string& error);

#endif // MCUFW_ARGUMENTS_HPP
