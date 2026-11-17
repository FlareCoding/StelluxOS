#include "arguments.hpp"

#include <cstdint>
#include <string_view>

// A command's name and the most operands it takes, with the sentence for too few
struct command_rule {
    std::string_view name;
    command_name     command;
    size_t           operands_max;
    std::string_view missing_operand;  // empty when the command needs none
};

constexpr command_rule COMMAND_RULES[] = {
    { "boards", command_name::boards, 1, "" },
    { "install", command_name::install, SIZE_MAX, "install needs a board, a core or a library, such as FastLED" },
    { "remove", command_name::remove, SIZE_MAX, "remove needs a core or a library, such as FastLED" },
};

static const command_rule* find_rule(std::string_view name) {
    for (const command_rule& rule : COMMAND_RULES) {
        if (rule.name == name) {
            return &rule;
        }
    }

    return nullptr;
}

bool parse_arguments(int argc, char** argv, arguments& parsed, std::string& error) {
    if (argc < 2) {
        error = "no command given";
        return false;
    }

    parsed = {};
    if (std::string_view(argv[1]) == "hook") {
        parsed.command = command_name::hook;
        parsed.operands.assign(argv + 2, argv + argc);
        return true;
    }

    const command_rule* rule = find_rule(argv[1]);
    if (!rule) {
        error = "unknown command " + std::string(argv[1]);
        return false;
    }

    parsed.command = rule->command;
    for (int index = 2; index < argc; index++) {
        std::string_view word = argv[index];
        if (word == "--events") {
            parsed.events = true;
        } else if (word.starts_with("-")) {
            error = std::string(rule->name) + " takes no option " + std::string(word);
            return false;
        } else {
            parsed.operands.emplace_back(word);
        }
    }

    if (parsed.operands.empty() && !rule->missing_operand.empty()) {
        error = rule->missing_operand;
        return false;
    }

    if (parsed.operands.size() > rule->operands_max) {
        error = std::string(rule->name) + " takes " + std::to_string(rule->operands_max) + " word at most";
        return false;
    }

    return true;
}
