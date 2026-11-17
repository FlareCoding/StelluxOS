/* hooks, run on the build machine: parse_hook's verbs and usage mistakes, and run_hook on temporary files, copying the
 * first source that exists, asking for `--else`, and leaving alone a file already holding what a verb writes */

#include "files.hpp"
#include "hooks.hpp"

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

// A time well in the past, which a file keeps unless a hook writes it
constexpr time_t OLD_SECONDS = 1000000000;

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

static std::string read_text(const std::string& path) {
    std::string text;
    std::string error;
    return read_file(path, text, error) ? text : "<unreadable>";
}

static void make_old(const std::string& path) {
    struct timeval times[2] = { { OLD_SECONDS, 0 }, { OLD_SECONDS, 0 } };
    utimes(path.c_str(), times);
}

static bool is_old(const std::string& path) {
    struct stat status;
    return stat(path.c_str(), &status) == 0 && status.st_mtime == OLD_SECONDS;
}

static void check_parsing() {
    hook_command command;
    std::string error;

    check(parse_hook({ "copy-first", "/b/out.csv", "/s/a.csv", "/v/b.csv" }, command, error) &&
              command.verb == hook_verb::copy_first && command.target == "/b/out.csv" &&
              command.sources == std::vector<std::string>({ "/s/a.csv", "/v/b.csv" }) && command.otherwise.empty(),
          "copy-first takes a target and its sources");
    check(parse_hook({ "copy-first", "/b/boot.bin", "/s/boot.bin", "--else", "python3", "-m", "esptool" }, command,
                     error) &&
              command.sources == std::vector<std::string>({ "/s/boot.bin" }) &&
              command.otherwise == std::vector<std::string>({ "python3", "-m", "esptool" }),
          "the words after --else are the command");
    check(parse_hook({ "write", "/b/flash_args", "--flash-mode dio", "0x1000 boot.bin" }, command, error) &&
              command.verb == hook_verb::write && command.target == "/b/flash_args" &&
              command.lines == std::vector<std::string>({ "--flash-mode dio", "0x1000 boot.bin" }),
          "write takes a file and lines, a line starting with -- among them");
    check(parse_hook({ "write", "/b/file_opts" }, command, error) && command.lines.empty(),
          "write may be given no lines");

    check(!parse_hook({}, command, error), "a verb is needed");
    check(!parse_hook({ "move", "/a", "/b" }, command, error) && error.find("move") != std::string::npos,
          "an unknown verb is named");
    check(!parse_hook({ "copy-first", "/b/out.csv" }, command, error), "copy-first needs a source");
    check(!parse_hook({ "copy-first", "/b/out.csv", "--else", "true" }, command, error),
          "copy-first needs a source before --else");
    check(!parse_hook({ "copy-first", "/b/out.csv", "/s/a.csv", "--else" }, command, error),
          "--else needs a command");
    check(!parse_hook({ "write" }, command, error), "write needs a file");
}

static void check_running(const std::string& root) {
    std::string target = join_path(root, "partitions.csv");
    std::string variant = join_path(root, "variant.csv");
    std::string fallback = join_path(root, "default.csv");
    write_text(variant, "variant\n");
    write_text(fallback, "default\n");

    hook_command command;
    std::string error;
    bool run_otherwise = true;
    parse_hook({ "copy-first", target, join_path(root, "sketch.csv"), variant, fallback }, command, error);
    check(run_hook(command, run_otherwise, error) && !run_otherwise && read_text(target) == "variant\n",
          "copy-first copies the first source that exists");

    make_old(target);
    check(run_hook(command, run_otherwise, error) && is_old(target), "copying the same content leaves the time");

    write_text(variant, "changed\n");
    check(run_hook(command, run_otherwise, error) && read_text(target) == "changed\n" && !is_old(target),
          "copying new content writes it");

    parse_hook({ "copy-first", target, join_path(root, "missing.csv"), "--else", "true" }, command, error);
    check(run_hook(command, run_otherwise, error) && run_otherwise, "no source asks for the --else command");

    parse_hook({ "copy-first", target, join_path(root, "missing.csv"), join_path(root, "gone.csv") }, command, error);
    check(!run_hook(command, run_otherwise, error) && error.find("gone.csv") != std::string::npos,
          "no source and no --else fails, naming the sources");

    std::string options = join_path(root, "file_opts");
    parse_hook({ "write", options, "-DARDUINO_CORE_BUILD" }, command, error);
    check(run_hook(command, run_otherwise, error) && read_text(options) == "-DARDUINO_CORE_BUILD\n",
          "write ends each line in a newline");

    make_old(options);
    check(run_hook(command, run_otherwise, error) && is_old(options), "writing the same lines leaves the time");

    parse_hook({ "write", options }, command, error);
    check(run_hook(command, run_otherwise, error) && read_text(options).empty(), "write of no lines empties the file");

    parse_hook({ "write", join_path(root, "missing/file") }, command, error);
    check(!run_hook(command, run_otherwise, error) && error.find("missing/file") != std::string::npos,
          "a file that cannot be written is named");
}

int main() {
    char root_template[] = "/tmp/mcufw-hooks-XXXXXX";
    std::string root = mkdtemp(root_template);

    check_parsing();
    check_running(root);

    std::string cleanup = "rm -rf " + root;
    if (system(cleanup.c_str()) != 0) {
        printf("hooks: could not delete %s\n", root.c_str());
    }

    if (g_failures != 0) {
        printf("hooks: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("hooks: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
