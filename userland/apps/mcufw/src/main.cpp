#include "arguments.hpp"
#include "commands.hpp"
#include "layout.hpp"
#include "report.hpp"

#include <cstdio>

constexpr int USAGE_STATUS = 2;
constexpr const char* USAGE =
    "usage: mcufw boards [<word>] [--events]\n"
    "       mcufw install <board, core or library>[@<version>]... [--events]\n"
    "       mcufw remove <core or library>... [--events]\n"
    "       mcufw build [<sketch>] [--board <board>] [--jobs <n>] [--warnings <level>] [--verbose] [--events]\n"
    "       mcufw clean [<sketch>] [--events]\n"
    "       mcufw hook copy-first <to> <from>... [--else <command>...]\n"
    "       mcufw hook write <file> [<line>...]\n";

int usage_error(const std::string& error) {
    fprintf(stderr, "mcufw: %s\n%s", error.c_str(), USAGE);
    return USAGE_STATUS;
}

int main(int argc, char** argv) {
    arguments parsed;
    std::string error;
    if (!parse_arguments(argc, argv, parsed, error)) {
        return usage_error(error);
    }

    reporter report(parsed.events, stdout, stderr);
    directory_layout layout = system_layout();
    switch (parsed.command) {
    case command_name::boards:
        return run_boards(parsed, layout, report);
    case command_name::install:
        return run_install(parsed, layout, report);
    case command_name::remove:
        return run_remove(parsed, layout, report);
    case command_name::hook:
        return run_hook_command(parsed, report);
    case command_name::build:
        return run_build(parsed, layout, report);
    case command_name::clean:
        return run_clean(parsed, layout, report);
    }
}
