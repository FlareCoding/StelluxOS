#include "arguments.hpp"
#include "commands.hpp"
#include "layout.hpp"
#include "report.hpp"

#include <cstdio>

constexpr int USAGE_STATUS = 2;
constexpr const char* USAGE =
    "usage: mcufw boards [<word>] [--events]\n"
    "       mcufw install <core or board>[@<version>]... [--events]\n"
    "       mcufw remove <core>... [--events]\n";

int main(int argc, char** argv) {
    arguments parsed;
    std::string error;
    if (!parse_arguments(argc, argv, parsed, error)) {
        fprintf(stderr, "mcufw: %s\n%s", error.c_str(), USAGE);
        return USAGE_STATUS;
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
    }
}
