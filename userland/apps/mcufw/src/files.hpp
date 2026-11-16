#ifndef MCUFW_FILES_HPP
#define MCUFW_FILES_HPP

// The file calls mcufw makes. Those taking `error` fail with a sentence naming the path and the error, and `is_file`
// and `is_directory` answer false whenever `stat` fails.

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

// The executable a command word runs, found as musl's `posix_spawnp` finds it: the word itself when it holds a slash,
// else the first match in `PATH`, or in musl's default list when `PATH` is unset. Empty when there is none.
std::string find_program(const std::string& word);

#endif // MCUFW_FILES_HPP
