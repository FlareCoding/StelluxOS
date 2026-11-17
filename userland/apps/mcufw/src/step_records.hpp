#ifndef MCUFW_STEP_RECORDS_HPP
#define MCUFW_STEP_RECORDS_HPP

// The record of what a finished build step ran and read, so a later build runs it again only when one of them changed.
// Hooks and the size recipe keep none and always run, as arduino-cli runs them, except a copy-first's `--else` command.

#include "build_plan.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The reason a step whose commands changed gets, which a caller may name the step in
constexpr std::string_view COMMAND_CHANGED_REASON = "its command changed";

struct file_stamp {
    std::string path;
    uint64_t    size;
    int64_t     modified_ns;
    std::string digest;  // a response file's SHA-256, whose time a hook may move without changing what it holds
};

struct step_record {
    std::vector<std::vector<std::string>> commands;
    std::vector<file_stamp>               files;
};

// The path of `step`'s record: `<object>.step`, `<archive>.step`, or for a link, an objcopy or a copy-first's `--else`
// command `<build>/steps/<recipe key>.step`. Empty for a step that always runs.
std::string step_record_path(const build_step& step, const std::string& build_directory);

// Whether `word` is `@` and an absolute path, a response file GCC reads more words from
bool is_response_word(std::string_view word);

// The dependency file GCC's `-MMD` writes beside a compile's object, its `.o` replaced by `.d`
std::string compile_depfile(const build_step& compile);

// The size and modification time of the regular file at `path`, and with `by_content` the SHA-256 of what it holds.
// False when there is no such file or it cannot be read.
bool stamp_file(const std::string& path, bool by_content, file_stamp& stamp);

// A stamp as one line of mcufw's records keeps it, `<size> <modified_ns> <digest or -> <path>`, without the newline
std::string format_file_stamp(const file_stamp& stamp);
bool parse_file_stamp(std::string_view text, file_stamp& stamp);

// The files a GCC dependency file names, its target left out
bool read_depfile(const std::string& path, std::vector<std::string>& files, std::string& error);

// Whether `record` still describes `step` and `current`, the stamps of the record's files now, naming the first
// difference in `reason` when not
bool record_matches(const step_record& record, const build_step& step, const std::vector<file_stamp>& current,
                    std::string& reason);

// Whether `step` finished before with the same commands and files, so it need not run. `reason` names what changed,
// and stays empty when there is no record to compare. `error` is set only for a record that exists but cannot be read.
bool step_is_current(const build_step& step, const std::string& build_directory, std::string& reason,
                     std::string& error);

// Records a step that succeeded with the files it read: a compile's from its .d file and its compiler, another's from
// its words, then its output, `@file`s by content. No .d file, no output, or an input changed during it leaves none.
bool record_finished_step(const build_step& step, const std::string& build_directory, int64_t started_ns,
                          std::string& error);

#endif // MCUFW_STEP_RECORDS_HPP
