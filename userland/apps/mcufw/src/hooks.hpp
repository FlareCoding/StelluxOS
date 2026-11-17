#ifndef MCUFW_HOOKS_HPP
#define MCUFW_HOOKS_HPP

// The `copy-first` and `write` verbs that stand in for a core's shell hooks. Neither touches a file whose content
// would not change, so a step that compares times sees no change.

#include <string>
#include <vector>

enum class hook_verb { copy_first, write };

struct hook_command {
    hook_verb                verb;
    std::string              target;     // the file copied to or written
    std::vector<std::string> sources;    // copy-first's, in order
    std::vector<std::string> lines;      // write's
    std::vector<std::string> otherwise;  // copy-first's `--else` command, empty without one
};

// Parses the words after `hook`, `copy-first <to> <from>... [--else <command>...]` or `write <file> [<line>...]`.
// A failure is a usage mistake.
bool parse_hook(const std::vector<std::string>& words, hook_command& command, std::string& error);

// Runs the verb. When copy-first finds no source, `run_otherwise` is set for a command with `--else`, and the call
// fails naming the sources for one without.
bool run_hook(const hook_command& command, bool& run_otherwise, std::string& error);

#endif // MCUFW_HOOKS_HPP
