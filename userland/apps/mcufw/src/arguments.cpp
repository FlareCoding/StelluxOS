#include "arguments.hpp"

#include <string_view>

constexpr size_t BOARDS_OPERANDS_MAX = 1;

bool parse_arguments(int argc, char** argv, arguments& parsed, std::string& error) {
    if (argc < 2) {
        error = "no command given";
        return false;
    }

    std::string_view command = argv[1];
    if (command != "boards") {
        error = "unknown command " + std::string(command);
        return false;
    }

    parsed = {};
    parsed.command = command_name::boards;
    for (int index = 2; index < argc; index++) {
        std::string_view word = argv[index];
        if (word == "--events") {
            parsed.events = true;
        } else if (word.starts_with("-")) {
            error = "boards takes no option " + std::string(word);
            return false;
        } else {
            parsed.operands.emplace_back(word);
        }
    }

    if (parsed.operands.size() > BOARDS_OPERANDS_MAX) {
        error = "boards takes one word at most";
        return false;
    }

    return true;
}
