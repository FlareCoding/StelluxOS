/* files, run on the build machine: path joining and splitting, reading whole files, directory listings sorted without
 * `.` and `..`, and finding a program as musl's posix_spawnp does */

#include "files.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static void write_text(const std::string& path, const std::string& text) {
    FILE* file = fopen(path.c_str(), "w");
    fputs(text.c_str(), file);
    fclose(file);
}

static void check_paths() {
    check(join_path("/var/lib", "mcufw") == "/var/lib/mcufw", "join_path adds one slash");
    check(join_path("/var/lib/", "mcufw") == "/var/lib/mcufw", "join_path keeps a slash already there");
    check(join_path("", "mcufw") == "mcufw", "join_path of no directory is the name");

    check(file_name("/a/b/boards.txt") == "boards.txt", "file_name takes the last part");
    check(file_name("boards.txt") == "boards.txt", "file_name of a bare name is the name");

    check(parent_directory("/a/b/boards.txt") == "/a/b", "parent_directory drops the last part");
    check(parent_directory("/boards.txt") == "/", "the root is its children's parent");
    check(parent_directory("boards.txt").empty(), "a bare name has no parent");
}

static void check_files(const std::string& root) {
    std::string path = join_path(root, "platform.txt");
    write_text(path, "name=ESP32 Arduino\n");

    std::string text;
    std::string error;
    check(read_file(path, text, error) && text == "name=ESP32 Arduino\n", "read_file reads every byte");
    check(!read_file(join_path(root, "missing.txt"), text, error), "read_file fails on a missing file");
    check(error.find("missing.txt") != std::string::npos, "the failure names the path");

    check(is_file(path) && !is_directory(path), "is_file accepts a file and is_directory refuses it");
    check(is_directory(root) && !is_file(root), "is_directory accepts a directory and is_file refuses it");
    check(!is_file(join_path(root, "missing.txt")), "a missing path is no file");
}

static void check_listing(const std::string& root) {
    std::string directory = join_path(root, "cores");
    mkdir(directory.c_str(), 0755);
    write_text(join_path(directory, "zeta"), "");
    write_text(join_path(directory, "alpha"), "");
    mkdir(join_path(directory, "middle").c_str(), 0755);

    std::vector<std::string> names;
    std::string error;
    check(list_directory(directory, names, error), "list_directory reads a directory");
    check(names == std::vector<std::string>({ "alpha", "middle", "zeta" }), "names come sorted, without dots");
    check(!list_directory(join_path(root, "missing"), names, error), "list_directory fails on a missing directory");
}

static void write_program(const std::string& path, mode_t mode) {
    write_text(path, "#!/bin/sh\n");
    chmod(path.c_str(), mode);
}

static void check_program_search(const std::string& root) {
    std::string first = join_path(root, "first");
    std::string second = join_path(root, "second");
    mkdir(first.c_str(), 0755);
    mkdir(second.c_str(), 0755);

    write_program(join_path(first, "both"), 0755);
    write_program(join_path(second, "both"), 0755);
    write_program(join_path(first, "later"), 0644);
    write_program(join_path(second, "later"), 0755);

    std::string path = first + ":" + second;
    setenv("PATH", path.c_str(), 1);
    check(find_program("both") == join_path(first, "both"), "the first PATH entry holding the program wins");
    check(find_program("later") == join_path(second, "later"), "a file that is not executable is passed over");
    check(find_program("absent").empty(), "a program no PATH entry holds is not found");
    check(find_program(join_path(second, "both")) == join_path(second, "both"), "a word with a slash is itself");

    unsetenv("PATH");
    check(find_program("sh") == "/bin/sh", "musl's default list is searched when PATH is unset");

    char working[PATH_MAX];
    std::string empty_first = ":" + second;
    setenv("PATH", empty_first.c_str(), 1);
    if (getcwd(working, sizeof(working)) && chdir(first.c_str()) == 0) {
        check(find_program("both") == "both", "an empty PATH entry names the working folder");
        check(chdir(working) == 0, "the working folder is restored");
    }
}

int main() {
    char root_template[] = "/tmp/mcufw-files-XXXXXX";
    std::string root = mkdtemp(root_template);
    std::string path = getenv("PATH") ? getenv("PATH") : "";

    check_paths();
    check_files(root);
    check_listing(root);
    check_program_search(root);
    setenv("PATH", path.c_str(), 1);

    std::string cleanup = "rm -rf " + root;
    if (system(cleanup.c_str()) != 0) {
        printf("files: could not delete %s\n", root.c_str());
    }

    if (g_failures != 0) {
        printf("files: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("files: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
