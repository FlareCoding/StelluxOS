#ifndef MCUFW_FILES_HPP
#define MCUFW_FILES_HPP

// The file calls mcufw makes. Those taking `error` fail with a sentence naming the path and the error, and `is_file`
// and `is_directory` answer false whenever `stat` fails.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

std::string join_path(std::string_view directory, std::string_view name);
std::string file_name(std::string_view path);
std::string parent_directory(std::string_view path);

bool read_file(const std::string& path, std::string& text, std::string& error);

bool is_directory(const std::string& path);
bool is_file(const std::string& path);

// The names in directory `path`, sorted, without `.` and `..`
bool list_directory(const std::string& path, std::vector<std::string>& names, std::string& error);

// The directories in directory `path`, sorted
bool list_subdirectories(const std::string& path, std::vector<std::string>& names, std::string& error);

// The executable a command word runs, found as musl's `posix_spawnp` finds it: the word itself when it holds a slash,
// else the first match in `PATH`, or in musl's default list when `PATH` is unset. Empty when there is none.
std::string find_program(const std::string& word);

// Creates each missing directory of `path`, outermost first
bool make_directories(const std::string& path, std::string& error);

// Deletes `path` and everything below it, a symbolic link itself and never what it names
bool remove_tree(const std::string& path, std::string& error);

// Creates a file that must not exist, for writing
bool create_new_file(const std::string& path, uint32_t mode, int& fd, std::string& error);

// Writes `text` into a new file at `path`, failing when a file is there already
bool write_new_file(const std::string& path, std::string_view text, std::string& error);

// Writes all of `bytes` to `fd`, which `path` names for errors
bool write_all(int fd, const std::string& path, std::string_view bytes, std::string& error);

// Renames `path` to `discard`, a free name on its filesystem, then deletes it there, so a delete cut short leaves
// nothing at `path`. A missing `path` is already gone.
bool discard_directory(const std::string& path, const std::string& discard, std::string& error);

// Moves the staged directory to `installed` by rename, what `installed` held going to `discard` first, then deletes
// that. Fails with `installed` already replaced when the old folder cannot be deleted.
bool replace_directory(const std::string& staged, const std::string& installed, const std::string& discard,
                       std::string& error);

#endif // MCUFW_FILES_HPP
