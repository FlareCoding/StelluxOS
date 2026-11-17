#ifndef MCUFW_COMMANDS_HPP
#define MCUFW_COMMANDS_HPP

// The commands, each run with its parsed arguments and returning the exit status, 0 on success and 1 on failure

#include "arguments.hpp"
#include "layout.hpp"
#include "report.hpp"

int run_boards(const arguments& parsed, const directory_layout& layout, reporter& report);
int run_install(const arguments& parsed, const directory_layout& layout, reporter& report);
int run_remove(const arguments& parsed, const directory_layout& layout, reporter& report);
int run_hook_command(const arguments& parsed, reporter& report);

// Prints `error` and mcufw's usage on stderr, and returns the status a usage mistake exits with
int usage_error(const std::string& error);

#endif // MCUFW_COMMANDS_HPP
