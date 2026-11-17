/* step records, run on the build machine in a temporary build tree: GCC's dependency files, where each step keeps its
 * record, and the changes that make a recorded compile or link run again */

#include "step_records.hpp"

#include "files.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS   = 8;
constexpr int64_t  NS_PER_SECOND = 1000000000;
constexpr int64_t  PAST_NS       = 1700000000 * NS_PER_SECOND;

static uint64_t    g_checks;
static uint64_t    g_failures;
static std::string g_root;

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

static std::string path_of(const std::string& name) {
    return g_root + "/" + name;
}

static void write_text(const std::string& name, const std::string& text, int64_t modified_ns) {
    std::string path = path_of(name);
    std::string error;
    FILE* file = make_directories(parent_directory(path), error) ? fopen(path.c_str(), "wb") : nullptr;
    if (!file || fwrite(text.data(), 1, text.size(), file) != text.size()) {
        printf("  cannot write %s\n", path.c_str());
        exit(1);
    }

    fclose(file);
    timespec times[2] = { { modified_ns / NS_PER_SECOND, modified_ns % NS_PER_SECOND },
                          { modified_ns / NS_PER_SECOND, modified_ns % NS_PER_SECOND } };
    utimensat(AT_FDCWD, path.c_str(), times, 0);
}

static void check_depfiles() {
    std::vector<std::string> files;
    std::string error;
    write_text("deps/escaped.d",
               "/b/sketch/My\\ Lamp.ino.cpp.o: /s/My\\ Lamp/My\\ Lamp.ino.cpp \\\r\n"
               " /s/lib/a\\#b.h /s/price$$.h \\\n  /s/back\\\\slash.h /s/keep\\q.h\n",
               PAST_NS);
    check(read_depfile(path_of("deps/escaped.d"), files, error) &&
              files == std::vector<std::string>({ "/s/My Lamp/My Lamp.ino.cpp", "/s/lib/a#b.h", "/s/price$.h",
                                                  "/s/back\\slash.h", "/s/keep\\q.h" }),
          "reads escaped spaces, hashes, dollars and backslashes across continued lines");

    write_text("deps/empty.d", "\n", PAST_NS);
    check(read_depfile(path_of("deps/empty.d"), files, error) && files.empty(), "reads an empty dependency file");

    write_text("deps/no-target.d", "/s/a.cpp /s/a.h\n", PAST_NS);
    bool refused = !read_depfile(path_of("deps/no-target.d"), files, error);
    check(refused && error.find("names no target") != std::string::npos, "refuses a dependency file without a target");

    write_text("deps/dollar.d", "a.o: /s/$x.h\n", PAST_NS);
    check(!read_depfile(path_of("deps/dollar.d"), files, error), "refuses a single dollar");
}

static build_step compile_step() {
    return { step_kind::compile, "recipe.cpp.o.pattern",
             { { path_of("bin/g++"), "-c", "@" + path_of("sdk/flags"), "@" + path_of("build/file_opts"),
                 path_of("src/Lamp.ino.cpp"), "-o", path_of("build/sketch/Lamp.ino.cpp.o") } },
             path_of("src/Lamp.ino.cpp"), path_of("build/sketch/Lamp.ino.cpp.o"), {} };
}

static void write_compile_tree(int64_t header_ns) {
    write_text("bin/g++", "compiler", PAST_NS);
    chmod(path_of("bin/g++").c_str(), 0755);
    write_text("sdk/flags", "-Os -mlongcalls", PAST_NS);
    write_text("build/file_opts", "", PAST_NS);
    write_text("src/Lamp.ino.cpp", "int main() {}", PAST_NS);
    write_text("src/Lamp.h", "#pragma once", header_ns);

    write_text("build/sketch/Lamp.ino.cpp.o", "object", PAST_NS + NS_PER_SECOND);
    write_text("build/sketch/Lamp.ino.cpp.d",
               path_of("build/sketch/Lamp.ino.cpp.o") + ": " + path_of("src/Lamp.ino.cpp") + " \\\n " +
                   path_of("src/Lamp.h") + "\n",
               PAST_NS + NS_PER_SECOND);
}

static bool is_current(const build_step& step, std::string& reason) {
    std::string error;
    bool current = step_is_current(step, path_of("build"), reason, error);
    return current && error.empty();
}

static void check_compile() {
    build_step step = compile_step();
    std::string build = path_of("build");
    std::string reason;
    std::string error;
    write_compile_tree(PAST_NS);
    check(!is_current(step, reason) && reason.empty(), "runs a compile that never finished");

    int64_t started_ns = PAST_NS + NS_PER_SECOND / 2;
    check(record_finished_step(step, build, started_ns, error), "records a finished compile");
    check(is_file(path_of("build/sketch/Lamp.ino.cpp.o.step")), "keeps the record beside the object");
    check(is_current(step, reason), "skips a compile whose files are unchanged");

    write_text("build/file_opts", "", PAST_NS + 5 * NS_PER_SECOND);
    check(is_current(step, reason), "skips it when a response file's time moved but not what it holds");

    write_text("build/file_opts", "-DARDUINO_CORE_BUILD", PAST_NS + 6 * NS_PER_SECOND);
    check(!is_current(step, reason) && reason == "file_opts changed", "runs it when a response file changed");
    write_text("build/file_opts", "", PAST_NS + 7 * NS_PER_SECOND);

    write_text("sdk/flags", "-O2 -mlongcalls", PAST_NS + 7 * NS_PER_SECOND);
    check(!is_current(step, reason) && reason == "flags changed",
          "runs it when a response file changed what it holds but not its size");
    write_text("sdk/flags", "-Os -mlongcalls", PAST_NS + 8 * NS_PER_SECOND);

    write_text("src/Lamp.h", "#pragma once", PAST_NS + 8 * NS_PER_SECOND);
    check(!is_current(step, reason) && reason == "Lamp.h changed", "runs it when a header changed");
    write_text("src/Lamp.h", "#pragma once", PAST_NS);

    build_step changed = step;
    changed.commands[0][1] = "-E";
    check(!is_current(changed, reason) && reason == "its command changed", "runs it when its command changed");

    write_text("bin/g++", "compilex", PAST_NS + 9 * NS_PER_SECOND);
    check(!is_current(step, reason) && reason == "g++ changed",
          "runs it when its compiler changed, though not its size");
    write_text("bin/g++", "compiler", PAST_NS);

    unlink(path_of("build/sketch/Lamp.ino.cpp.o").c_str());
    check(!is_current(step, reason) && reason == "Lamp.ino.cpp.o is gone", "runs it when its object is gone");

    write_text("src/Lamp.ino.cpp", "int main() { return 1; }", PAST_NS + 9 * NS_PER_SECOND);
    check(!is_current(step, reason) && reason == "Lamp.ino.cpp changed",
          "names the edited source before the object a failed compile deleted");
}

static void check_edit_during_compile() {
    build_step step = compile_step();
    std::string build = path_of("build");
    std::string reason;
    std::string error;
    unlink(path_of("build/sketch/Lamp.ino.cpp.o.step").c_str());

    int64_t started_ns = PAST_NS + 20 * NS_PER_SECOND;
    write_compile_tree(started_ns);
    check(record_finished_step(step, build, started_ns, error),
          "succeeds for a compile whose header changed during it");
    check(!is_file(path_of("build/sketch/Lamp.ino.cpp.o.step")), "keeps no record for it");
    check(!is_current(step, reason), "compiles it again on the next build");

    unlink(path_of("build/sketch/Lamp.ino.cpp.d").c_str());
    check(record_finished_step(step, build, PAST_NS + 30 * NS_PER_SECOND, error) &&
              !is_file(path_of("build/sketch/Lamp.ino.cpp.o.step")),
          "succeeds and keeps no record for a compile that wrote no .d file");

    write_compile_tree(PAST_NS);
    unlink(path_of("build/sketch/Lamp.ino.cpp.o").c_str());
    check(record_finished_step(step, build, PAST_NS + 30 * NS_PER_SECOND, error) &&
              !is_file(path_of("build/sketch/Lamp.ino.cpp.o.step")),
          "succeeds and keeps no record for a compile that wrote no object");
}

static void check_link() {
    std::string build = path_of("build");
    std::string reason;
    std::string error;

    write_text("build/sketch/Lamp.ino.cpp.o", "object", PAST_NS);
    write_text("build/core/core.a", "archive", PAST_NS);
    write_text("sdk/ld_flags", "-Wl,--gc-sections", PAST_NS);
    write_text("build/Lamp.ino.elf", "elf", PAST_NS + NS_PER_SECOND);
    build_step link = { step_kind::link, "recipe.c.combine.pattern",
                        { { path_of("bin/g++"), "@" + path_of("sdk/ld_flags"), path_of("build/sketch/Lamp.ino.cpp.o"),
                            path_of("build/core/core.a"), "-o", path_of("build/Lamp.ino.elf") } },
                        "", path_of("build/Lamp.ino.elf"), {} };

    check(record_finished_step(link, build, PAST_NS + 2 * NS_PER_SECOND, error), "records a link");
    check(step_record_path(link, build) == path_of("build/steps/recipe.c.combine.pattern.step"),
          "keeps a link's record under its recipe key");
    check(is_current(link, reason), "skips a link whose objects are unchanged");

    write_text("build/Lamp.ino.elf", "elf", PAST_NS + 3 * NS_PER_SECOND);
    check(!is_current(link, reason) && reason == "Lamp.ino.elf changed", "runs it again when its output changed");

    build_step otherwise = { step_kind::hook, "recipe.hooks.prebuild.4.pattern",
                             { { path_of("bin/esptool"), "elf2image", path_of("sdk/bootloader.elf") } }, "",
                             path_of("build/Lamp.ino.bootloader.bin"), {} };
    write_text("bin/esptool", "tool", PAST_NS);
    write_text("sdk/bootloader.elf", "elf", PAST_NS);
    check(record_finished_step(otherwise, build, PAST_NS + NS_PER_SECOND, error) && !is_current(otherwise, reason),
          "records no `--else` command whose target is missing, so it runs again");

    write_text("build/Lamp.ino.bootloader.bin", "image", PAST_NS);
    check(record_finished_step(otherwise, build, PAST_NS + NS_PER_SECOND, error) && is_current(otherwise, reason),
          "records an `--else` command with the target its words leave out");

    unlink(path_of("build/Lamp.ino.bootloader.bin").c_str());
    check(!is_current(otherwise, reason) && reason == "Lamp.ino.bootloader.bin is gone",
          "runs it again once its target is gone");
}

static void check_paths() {
    std::string build = "/var/cache/mcufw/builds/Lamp-0a1b2c3d";
    build_step archive = { step_kind::archive, "recipe.ar.pattern", {}, "", build + "/libraries/FastLED/FastLED.a",
                           {} };
    build_step otherwise = { step_kind::hook, "recipe.hooks.prebuild.4.pattern", {}, "",
                             build + "/Lamp.ino.bootloader.bin", {} };
    build_step hook = { step_kind::hook, "recipe.hooks.prebuild.1.pattern", {}, "", "", {} };
    build_step size = { step_kind::size, "recipe.size.pattern", {}, "", "", {} };

    check(step_record_path(archive, build) == build + "/libraries/FastLED/FastLED.a.step",
          "keeps an archive's record beside it");
    check(step_record_path(otherwise, build) == build + "/steps/recipe.hooks.prebuild.4.pattern.step",
          "keeps a copy-first's `--else` command's record under its hook's key");
    check(step_record_path(hook, build).empty() && step_record_path(size, build).empty(),
          "keeps no record for a hook or the size recipe");

    std::string error;
    check(record_finished_step(hook, build, 0, error), "records nothing for a step that always runs");
}

static void check_matching() {
    build_step step = { step_kind::objcopy, "recipe.objcopy.bin.pattern", { { "python3", "-m", "esptool" } }, "", "",
                        {} };
    step_record record = { step.commands, { { "/b/Lamp.ino.elf", 10, 5, "" }, { "/b/file_opts", 0, 7, "e3b0" } } };
    std::vector<file_stamp> rewritten = { { "/b/Lamp.ino.elf", 10, 5, "" }, { "/b/file_opts", 0, 9, "e3b0" } };
    std::vector<file_stamp> moved = { { "/b/Lamp.ino.elf", 10, 6, "" }, { "/b/file_opts", 0, 7, "e3b0" } };
    std::vector<file_stamp> missing = { { "/b/Lamp.ino.elf", 10, 5, "" } };

    std::string reason;
    check(record_matches(record, step, rewritten, reason),
          "matches by size and time, and by content for a response file");
    check(!record_matches(record, step, moved, reason) && reason == "Lamp.ino.elf changed",
          "names the first file whose time moved");
    check(!record_matches(record, step, missing, reason) && reason == "file_opts is gone",
          "names a file it has no stamp for");

    build_step other = step;
    other.commands[0][2] = "esptool.py";
    std::vector<file_stamp> unchanged = { { "/b/Lamp.ino.elf", 10, 5, "" }, { "/b/file_opts", 0, 7, "e3b0" } };
    check(!record_matches(record, other, unchanged, reason) && reason == COMMAND_CHANGED_REASON,
          "names a changed command first");

    file_stamp parsed;
    check(parse_file_stamp("12 34 - /path/with spaces/x.h", parsed) && parsed.size == 12 && parsed.modified_ns == 34 &&
              parsed.digest.empty() && parsed.path == "/path/with spaces/x.h",
          "reads a stamp whose path holds spaces");
    check(!parse_file_stamp("12 x - /a", parsed) && !parse_file_stamp("12 34 -", parsed), "refuses a damaged stamp");
    check(format_file_stamp({ "/a b", 1, 2, "ff" }) == "1 2 ff /a b", "writes a stamp as it reads one");

    write_text("build/steps/recipe.objcopy.bin.pattern.step", "command\nword python3\ngarbage\n", PAST_NS);
    build_step objcopy = { step_kind::objcopy, "recipe.objcopy.bin.pattern", { { "python3" } }, "", "", {} };
    check(!is_current(objcopy, reason) && reason.empty(), "runs a step whose record is damaged");
}

int main() {
    char root[] = "/tmp/mcufw-steps-XXXXXX";
    if (!mkdtemp(root)) {
        printf("  cannot make a temporary folder\n");
        return 1;
    }

    g_root = root;
    check_depfiles();
    check_compile();
    check_edit_during_compile();

    check_link();
    check_paths();
    check_matching();

    std::string error;
    if (!remove_tree(g_root, error)) {
        printf("  %s\n", error.c_str());
    }

    if (g_failures != 0) {
        printf("step_records: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("step_records: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
