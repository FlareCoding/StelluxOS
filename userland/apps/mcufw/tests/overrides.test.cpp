/* The Stellux overrides, run on the build machine against fixtures/hardware: a core's overrides layered between its
 * platform.txt and platform.local.txt, a core with overrides only for other versions, and every recipe's tools */

#include "boards.hpp"
#include "cores.hpp"
#include "files.hpp"
#include "fqbn.hpp"
#include "requirements.hpp"
#include "tools.hpp"

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

static void check(bool holds, const std::string& what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what.c_str());
    }
}

static void write_text(const std::string& path, const std::string& text) {
    std::string error;
    bool changed = false;
    check(make_directories(parent_directory(path), error) && write_file_if_changed(path, text, changed, error),
          "wrote " + path);
}

static directory_layout layout_with(const std::string& overrides) {
    directory_layout layout = {};
    layout.hardware = "fixtures/hardware";
    layout.data_tools = "fixtures/tools";
    layout.program_tools = "fixtures/program-tools";
    layout.overrides = overrides;
    return layout;
}

static const installed_core* native_core(const std::vector<installed_core>& cores) {
    for (const installed_core& core : cores) {
        if (core.architecture == "native") {
            return &core;
        }
    }

    return nullptr;
}

static board_requirements plain_requirements(const std::vector<installed_core>& cores, const directory_layout& layout) {
    board_request request;
    selected_board board;
    std::string error;
    bool selected = parse_board_request("stellux:native:plain", request, error) &&
                    select_board(cores, request, board, error);
    check(selected, "the plain board selects: " + error);
    return selected ? check_requirements(board, locate_tools(board.core->tools, layout)) : board_requirements{};
}

static void check_layering(const std::string& root) {
    std::string overrides = join_path(root, "layered");
    write_text(join_path(overrides, "stellux/native/1.0.0.txt"),
               "name=Stellux Native, ported\n"
               "build.extra_flags=-DOVERRIDDEN\n"
               "recipe.objcopy.bin.pattern=\"{runtime.tools.gamma-tool.path}/pack\"\n"
               "upload.tool.default=uploader\n"
               "tools.uploader.path={runtime.tools.beta-sdk.path}\n"
               "tools.uploader.upload.pattern=\"{path}/upload\"\n");

    std::vector<installed_core> cores;
    std::string error;
    directory_layout layout = layout_with(overrides);
    check(load_installed_cores(layout, cores, error), "the cores load with overrides: " + error);

    const installed_core* core = native_core(cores);
    check(core && core->platform.get("name") == "Stellux Native, ported", "the overrides replace platform.txt's keys");
    check(core && core->platform.get("build.extra_flags") == "-DSTELLUX_LOCAL", "platform.local.txt has the last word");
    check(core && core->newest_overridden_version.empty(), "a core with overrides for its version needs no other");

    board_requirements requirements = plain_requirements(cores, layout);
    check(describe_missing(requirements) == "gamma-tool, stellux:beta-sdk 2.0",
          "every recipe's tools are needed, and the upload tool's through its own properties: " +
              describe_missing(requirements));
}

static void check_other_versions(const std::string& root) {
    std::string overrides = join_path(root, "newer");
    write_text(join_path(overrides, "stellux/native/1.5.0.txt"), "compiler.cpp.cmd=clang++\n");
    write_text(join_path(overrides, "stellux/native/2.0.0.txt"), "compiler.cpp.cmd=clang++\n");
    write_text(join_path(overrides, "esp32/esp32/3.3.12.txt"), "compiler.ar.cmd=ar\n");

    std::vector<installed_core> cores;
    std::string error;
    directory_layout layout = layout_with(overrides);
    check(load_installed_cores(layout, cores, error), "the cores load: " + error);

    const installed_core* core = native_core(cores);
    check(core && core->newest_overridden_version == "2.0.0", "the newest overridden version is kept");
    check(core && core->platform.get("compiler.cpp.cmd") == "g++", "no other version's overrides apply");

    board_requirements requirements = plain_requirements(cores, layout);
    check(requirements.missing_overrides == "stellux:native 2.0.0" &&
              describe_missing(requirements) == "stellux:native 2.0.0",
          "the version with overrides is what the board needs: " + describe_missing(requirements));

    requirements.missing_program = "native-g++";
    check(describe_missing(requirements) == "stellux:native 2.0.0, native-g++",
          "the core's version comes before a missing program: " + describe_missing(requirements));

    layout = layout_with(join_path(root, "elsewhere"));
    write_text(join_path(root, "elsewhere/esp32/esp32/3.3.12.txt"), "compiler.ar.cmd=ar\n");
    check(load_installed_cores(layout, cores, error) && native_core(cores) &&
              native_core(cores)->newest_overridden_version.empty(),
          "a core without overrides of its own loads as it is");
}

static void check_unreadable(const std::string& root) {
    if (geteuid() == 0) {
        return;
    }

    std::string folder = join_path(root, "locked/stellux/native");
    write_text(join_path(folder, "1.0.0.txt"), "compiler.cpp.cmd=clang++\n");
    chmod(folder.c_str(), 0);

    std::vector<installed_core> cores;
    std::string error;
    check(!load_installed_cores(layout_with(join_path(root, "locked")), cores, error),
          "an overrides folder that cannot be read fails the load");
    chmod(folder.c_str(), 0755);
}

int main() {
    char made[] = "/tmp/mcufw-overrides-XXXXXX";
    char resolved[PATH_MAX];
    if (!mkdtemp(made) || !realpath(made, resolved)) {
        printf("overrides: cannot make a temporary folder\n");
        return 1;
    }

    std::string root = resolved;
    check_layering(root);
    check_other_versions(root);
    check_unreadable(root);
    std::string remove = "rm -rf " + root;
    check(system(remove.c_str()) == 0, "the temporary folder is removed");

    if (g_failures != 0) {
        printf("overrides: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("overrides: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
