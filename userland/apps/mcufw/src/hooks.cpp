#include "hooks.hpp"

#include "files.hpp"

constexpr const char* OTHERWISE_WORD = "--else";

static bool parse_copy_first(const std::vector<std::string>& words, hook_command& command, std::string& error) {
    size_t word = 1;
    for (; word < words.size() && words[word] != OTHERWISE_WORD; word++) {
        if (word == 1) {
            command.target = words[word];
        } else {
            command.sources.push_back(words[word]);
        }
    }

    if (command.sources.empty()) {
        error = "copy-first needs a file to copy to and at least one file to copy from";
        return false;
    }

    if (word < words.size()) {
        command.otherwise.assign(words.begin() + static_cast<ptrdiff_t>(word) + 1, words.end());
        if (command.otherwise.empty()) {
            error = "copy-first's --else needs a command";
            return false;
        }
    }

    return true;
}

bool parse_hook(const std::vector<std::string>& words, hook_command& command, std::string& error) {
    command = {};
    if (words.empty()) {
        error = "hook needs a verb, copy-first or write";
        return false;
    }

    if (words[0] == "copy-first") {
        command.verb = hook_verb::copy_first;
        return parse_copy_first(words, command, error);
    }

    if (words[0] == "write") {
        if (words.size() < 2) {
            error = "write needs a file";
            return false;
        }

        command.verb = hook_verb::write;
        command.target = words[1];
        command.lines.assign(words.begin() + 2, words.end());
        return true;
    }

    error = "hook has no verb " + words[0] + ", only copy-first and write";

    return false;
}

static std::string describe_sources(const std::vector<std::string>& sources) {
    std::string described;
    for (size_t i = 0; i < sources.size(); i++) {
        if (i > 0) {
            described += i + 1 == sources.size() ? " or " : ", ";
        }

        described += sources[i];
    }

    return described;
}

bool run_hook(const hook_command& command, bool& run_otherwise, std::string& error) {
    run_otherwise = false;
    bool changed = false;
    if (command.verb == hook_verb::write) {
        std::string text;
        for (const std::string& line : command.lines) {
            text += line;
            text += '\n';
        }

        return write_file_if_changed(command.target, text, changed, error);
    }

    for (const std::string& source : command.sources) {
        if (is_file(source)) {
            return copy_file_if_changed(source, command.target, changed, error);
        }
    }

    if (command.otherwise.empty()) {
        error = "nothing to copy to " + command.target + ", since there is no " + describe_sources(command.sources);
        return false;
    }

    run_otherwise = true;

    return true;
}
