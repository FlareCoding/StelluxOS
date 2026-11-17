/* build_plan, run on the build machine against fixtures/compile: the commands planned for the Lamp sketch, in the
 * order of the build's stages, are the ones arduino-cli 1.6.0 runs for it, and the planning rules hold */

#include "boards.hpp"
#include "build_plan.hpp"
#include "cores.hpp"
#include "files.hpp"
#include "fqbn.hpp"
#include "sketch.hpp"
#include "tools.hpp"

#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

constexpr const char* FIXTURE_HARDWARE = "fixtures/compile/hardware";
constexpr const char* FIXTURE_CORE = "fixtures/compile/hardware/stellux/gcc/1.0.0";
constexpr const char* FIXTURE_SKETCH = "fixtures/compile/sketches/Lamp";
constexpr const char* EXPECTED_COMMANDS = "fixtures/compile/commands.txt";

// Planning reads no build tree, so the tree's path need not exist
constexpr const char* BUILD_TREE = "/mcufw-test/builds/Lamp-0123abcd";

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

static std::string absolute(const char* path) {
    char resolved[PATH_MAX];
    return realpath(path, resolved) ? resolved : path;
}

static void replace_all(std::string& text, const std::string& from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

static std::vector<std::string> expected_commands(const sketch& lamp, const std::string& build_tree) {
    std::string text;
    std::string error;
    check(read_file(EXPECTED_COMMANDS, text, error), error);

    replace_all(text, "{core}", absolute(FIXTURE_CORE));
    replace_all(text, "{build}", build_tree);
    replace_all(text, "{sketch}", lamp.folder);
    replace_all(text, "\t", " ");

    std::vector<std::string> commands;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        commands.push_back(text.substr(start, end - start));
        start = end == std::string::npos ? text.size() : end + 1;
    }

    return commands;
}

static std::string joined(const std::vector<std::string>& words) {
    std::string command;
    for (const std::string& word : words) {
        command += (command.empty() ? "" : " ") + word;
    }

    return command;
}

static void add_commands(const std::vector<build_step>& steps, std::vector<std::string>& commands) {
    for (const build_step& step : steps) {
        for (const std::vector<std::string>& words : step.commands) {
            commands.push_back(joined(words));
        }
    }
}

static std::vector<std::string> objects_of(const std::vector<build_step>& steps) {
    std::vector<std::string> objects;
    for (const build_step& step : steps) {
        objects.push_back(step.output);
    }

    std::sort(objects.begin(), objects.end());

    return objects;
}

static bool load_lamp(std::vector<installed_core>& cores, property_table& properties, sketch& lamp,
                      const std::string& build_tree = BUILD_TREE) {
    std::string error;
    directory_layout layout = {};
    layout.hardware = absolute(FIXTURE_HARDWARE);

    board_request request;
    selected_board board;
    bool loaded = load_installed_cores(layout, cores, error) &&
                  parse_board_request("stellux:gcc:lamp", request, error) &&
                  select_board(cores, request, board, error) && load_sketch(FIXTURE_SKETCH, lamp, error);
    check(loaded, "the fixture loads: " + error);
    if (!loaded) {
        return false;
    }

    // The reference run, on a machine without Stellux's /usr/gcc, passed --build-property compiler.path=
    property_table on_build_machine = board_properties(board, locate_tools(board.core->tools, layout));
    on_build_machine.set("compiler.path", "");
    properties = build_properties(on_build_machine, lamp, build_tree);

    return true;
}

// A build tree holding the sources a Lamp build copies, which planning the compiles walks
static std::string make_lamp_tree() {
    char root_template[] = "/tmp/mcufw-build-plan-XXXXXX";
    std::string root = absolute(mkdtemp(root_template));
    std::string error;
    for (const char* source : { "sketch/Lamp.ino.cpp", "sketch/helpers.cpp", "sketch/src/effects/fade.cpp" }) {
        std::string path = join_path(root, source);
        check(make_directories(parent_directory(path), error) && write_new_file(path, "", error), error);
    }

    return root;
}

// One stage's commands as the build plans them, at arduino-cli's default level so every command can match
static bool plan_stage(const property_table& properties, const stage_step& stage, compile_plan& compiles,
                       std::vector<build_step>& steps, std::string& error) {
    std::string core = absolute(FIXTURE_CORE);
    std::string core_archive = join_path(properties.get("build.path"), "core/core.a");
    std::vector<std::string> includes = { join_path(core, "cores/gcc"), join_path(core, "variants/lamp") };
    std::vector<build_step> folder_archives;
    build_step step;
    switch (stage.stage) {
    case build_stage::hooks:
        return plan_hooks(properties, stage.hook_point, steps, error);
    case build_stage::sketch_tree:
        return true;
    case build_stage::sketch_compiles:
        if (!plan_build_compiles(properties, includes, "none", compiles, error)) {
            return false;
        }

        steps.insert(steps.end(), compiles.sketch.begin(), compiles.sketch.end());
        steps.insert(steps.end(), compiles.sources.begin(), compiles.sources.end());
        return true;
    case build_stage::core_compiles:
        steps.insert(steps.end(), compiles.variant.begin(), compiles.variant.end());
        steps.insert(steps.end(), compiles.core.begin(), compiles.core.end());
        return true;
    case build_stage::core_archive:
        steps.push_back(step);
        return plan_archive(properties, objects_of(compiles.core), core_archive, steps.back(), error);
    case build_stage::link:
        if (!plan_link(properties, linked_objects(compiles), core_archive, "none", folder_archives, step, error)) {
            return false;
        }

        steps.insert(steps.end(), folder_archives.begin(), folder_archives.end());
        steps.push_back(step);
        return true;
    case build_stage::objcopy:
        return plan_objcopy(properties, steps, error);
    }

    return false;
}

// The commands of every stage, in the order the build runs its stages, are the ones arduino-cli runs
static void check_arduino_commands() {
    std::string build_tree = make_lamp_tree();
    std::vector<installed_core> cores;
    property_table properties;
    sketch lamp;
    if (!load_lamp(cores, properties, lamp, build_tree)) {
        return;
    }

    compile_plan compiles;
    std::vector<build_step> steps;
    std::string error;
    for (const stage_step& stage : build_stages()) {
        check(plan_stage(properties, stage, compiles, steps, error), "every stage plans: " + error);
    }

    std::vector<std::string> commands;
    add_commands(steps, commands);
    std::vector<std::string> expected = expected_commands(lamp, build_tree);
    check(commands.size() == expected.size(),
          "as many commands as arduino-cli runs: " + std::to_string(commands.size()));
    for (size_t i = 0; i < commands.size() && i < expected.size(); i++) {
        check(commands[i] == expected[i], "command " + std::to_string(i + 1) + " is arduino-cli's: " + commands[i]);
    }

    auto made = [&](step_kind kind, const std::string& output) {
        return std::any_of(steps.begin(), steps.end(), [&](const build_step& step) {
            return step.kind == kind && step.output == output;
        });
    };

    check(made(step_kind::link, join_path(build_tree, "Lamp.ino.elf")) &&
              made(step_kind::archive, join_path(build_tree, "core/core.a")),
          "the link makes the ELF and the archive step core.a");
    remove_tree(build_tree, error);
}

// The variant and the core compile at `none` whatever level the sketch takes, and the link takes the sketch's
static void check_group_levels() {
    std::string build_tree = make_lamp_tree();
    std::vector<installed_core> cores;
    property_table properties;
    sketch lamp;
    if (!load_lamp(cores, properties, lamp, build_tree)) {
        return;
    }

    compile_plan compiles;
    std::string error;
    check(plan_build_compiles(properties, {}, "all", compiles, error), "the compiles plan at all: " + error);

    auto has_word = [](const build_step& step, const char* word) {
        return std::find(step.commands[0].begin(), step.commands[0].end(), word) != step.commands[0].end();
    };

    check(!compiles.sketch.empty() && !compiles.sources.empty() && has_word(compiles.sketch[0], "-Wextra") &&
              has_word(compiles.sources[0], "-Wextra"),
          "the sketch's files and its src/ folder's take the chosen level");
    check(!compiles.variant.empty() && !compiles.core.empty() && has_word(compiles.variant[0], "-w") &&
              has_word(compiles.core[0], "-w") && !has_word(compiles.core[0], "-Wextra"),
          "the variant and the core take none");

    std::vector<build_step> folder_archives;
    build_step link;
    check(plan_link(properties, linked_objects(compiles), "/core.a", "all", folder_archives, link, error) &&
              link.commands.size() == 1 && std::find(link.commands[0].begin(), link.commands[0].end(), "-Wextra") !=
                                               link.commands[0].end(),
          "the link takes the chosen level");

    std::vector<std::string> objects = linked_objects(compiles);
    check(objects.size() == 4 && objects[0].ends_with("sketch/Lamp.ino.cpp.o") &&
              objects[2].ends_with("sketch/src/effects/fade.cpp.o") && objects[3].ends_with("core/variant.cpp.o"),
          "the link takes the sketch's objects, then src/'s, then the variant's");

    check(compiles_to_start(5, 2, false) == 2 && compiles_to_start(1, 4, false) == 1 &&
              compiles_to_start(5, 2, true) == 0,
          "compiles start up to the free slots, and none after a failure");

    remove_tree(build_tree, error);
}

static void check_warnings_and_names() {
    std::vector<installed_core> cores;
    property_table properties;
    sketch lamp;
    if (!load_lamp(cores, properties, lamp)) {
        return;
    }

    std::string sketch_tree = join_path(BUILD_TREE, "sketch");
    std::vector<build_step> steps;
    std::string error;

    plan_compiles(properties, { join_path(sketch_tree, "helpers.cpp") }, sketch_tree, sketch_tree, {}, "default", steps,
                  error);
    plan_compiles(properties, { join_path(sketch_tree, "helpers.cpp") }, sketch_tree, sketch_tree, {}, "all", steps,
                  error);
    check(steps.size() == 2, "both levels plan");
    if (steps.size() == 2) {
        const std::vector<std::string>& quiet = steps[0].commands[0];
        const std::vector<std::string>& loud = steps[1].commands[0];
        check(std::find(quiet.begin(), quiet.end(), "-w") == quiet.end(), "the default level adds no -w");
        check(std::find(loud.begin(), loud.end(), "-Wextra") != loud.end(), "the all level adds its flags");
    }

    steps.clear();
    check(plan_compiles(properties, { join_path(sketch_tree, "a.cxx"), join_path(sketch_tree, "b.cc") }, sketch_tree,
                        sketch_tree, {}, "none", steps, error) &&
              steps.size() == 2 && steps[0].recipe == "recipe.cpp.o.pattern" &&
              steps[1].recipe == "recipe.cpp.o.pattern",
          "a .cxx and a .cc source fall back to the C++ recipe when the core has none of their own");

    properties.set("recipe.cc.o.pattern", "");
    steps.clear();
    check(!plan_compiles(properties, { join_path(sketch_tree, "b.cc") }, sketch_tree, sketch_tree, {}, "none", steps,
                         error),
          "a .cc source takes the core's own recipe when it sets one, even empty");
    check(!plan_compiles(properties, { "/elsewhere/c.cpp" }, sketch_tree, sketch_tree, {}, "none", steps, error),
          "a source outside its root fails");

    check(!plan_compiles(properties, { join_path(sketch_tree, "d.S") }, sketch_tree, sketch_tree, {}, "none", steps,
                         error),
          "a .S source without its recipe fails");

    std::vector<build_step> hooks;
    properties.set("recipe.hooks.postbuild.pattern", "");
    properties.set("recipe.hooks.postbuild.1.pattern", "");
    check(plan_hooks(properties, "postbuild", hooks, error) && hooks.empty(), "an empty hook recipe runs nothing");

    build_step link;
    std::vector<build_step> folder_archives;
    properties.set("recipe.c.combine.pattern", "");
    check(plan_link(properties, { join_path(sketch_tree, "a.o") }, "/core.a", "none", folder_archives, link, error) &&
              link.commands.empty(),
          "without a combine recipe the link runs nothing, as arduino-cli skips it");

    directory_layout layout = {};
    layout.builds = "/var/cache/mcufw/builds";
    std::string tree = build_directory_for(layout, "/sketches/My Lamp");
    std::string prefix = "/var/cache/mcufw/builds/My Lamp-";
    check(tree.rfind(prefix, 0) == 0 && tree.size() == prefix.size() + 8 &&
              tree.find_first_not_of("0123456789abcdef", prefix.size()) == std::string::npos,
          "a build tree is named for its folder and 8 hex digits of its path's SHA-256");
    check(build_directory_for(layout, "/sketches/My Lamp") == tree &&
              build_directory_for(layout, "/other/My Lamp") != tree,
          "the same folder names the same tree, and another folder of the same name another");
}

// Objects whose quoted list, as the link names them, is exactly `length` characters
static std::vector<std::string> objects_of_length(size_t length) {
    std::string folder = join_path(BUILD_TREE, "sketch/");
    std::vector<std::string> objects;
    size_t total = 0;
    while (true) {
        std::string object = folder + "part" + std::to_string(objects.size()) + ".o";
        size_t separator = objects.empty() ? 0 : 1;
        if (total + separator + object.size() + 2 + folder.size() + 8 > length) {
            size_t padding = length - total - separator - 2 - folder.size() - 2;
            objects.push_back(folder + std::string(padding, 'x') + ".o");
            return objects;
        }

        objects.push_back(object);
        total += separator + object.size() + 2;
    }
}

static void check_words() {
    std::vector<installed_core> cores;
    property_table properties;
    sketch lamp;
    if (!load_lamp(cores, properties, lamp)) {
        return;
    }

    std::string sketch_tree = join_path(BUILD_TREE, "sketch");
    std::vector<build_step> steps;
    std::string error;
    check(plan_compiles(properties, { join_path(sketch_tree, "helpers.cpp") }, sketch_tree, sketch_tree,
                        { "/libraries/My Lib" }, "none", steps, error) &&
              std::find(steps[0].commands[0].begin(), steps[0].commands[0].end(), "-I/libraries/My Lib") !=
                  steps[0].commands[0].end(),
          "an include folder with a space stays one word");

    build_step link;
    std::vector<build_step> folder_archives;
    properties.set("recipe.c.combine.pattern", "ld \"{archive_file}\"");
    check(plan_link(properties, {}, join_path(BUILD_TREE, "core/core.a"), "none", folder_archives, link, error) &&
              link.commands.size() == 1 && link.commands[0].back() == "core/core.a",
          "archive_file names the core's archive relative to the build tree");

    check(plan_link(properties, objects_of_length(30000), "/core.a", "none", folder_archives, link, error) &&
              folder_archives.empty(),
          "30,000 characters of objects link directly");
    std::vector<std::string> longer = objects_of_length(30001);
    check(plan_link(properties, longer, "/core.a", "none", folder_archives, link, error) &&
              folder_archives.size() == 1 && folder_archives[0].commands.size() == longer.size(),
          "one more character archives them first");

    build_step archive;
    properties.set("recipe.ar.pattern", "ar rcs {archive_file} {object_file}");
    check(plan_archive(properties, { "/b/core/a.o" }, "/b/core/core.a", archive, error) &&
              archive.commands[0][2] == "core.a",
          "an archive recipe's archive_file is the archive's name, as arduino-cli sets it");
}

static void check_long_link() {
    std::vector<installed_core> cores;
    property_table properties;
    sketch lamp;
    if (!load_lamp(cores, properties, lamp)) {
        return;
    }

    std::vector<std::string> objects;
    std::string library_tree = join_path(BUILD_TREE, "libraries/Big");
    for (int i = 0; i < 400; i++) {
        objects.push_back(join_path(library_tree, "src/part_with_a_long_name_" + std::to_string(i) + ".cpp.o"));
    }

    objects.push_back(join_path(BUILD_TREE, "sketch/Lamp.ino.cpp.o"));
    objects.push_back(join_path(BUILD_TREE, "libraries/Dots/Dots.a"));

    objects.push_back(join_path(BUILD_TREE, "sketch/src/nested.cpp.o"));

    build_step link;
    std::vector<build_step> archives;
    std::string error;
    check(plan_link(properties, objects, join_path(BUILD_TREE, "core/core.a"), "none", archives, link, error),
          "a long link plans");
    check(archives.size() == 3 && link.commands.size() == 1, "an archive step per folder, then the link");

    const std::vector<std::string>& first = archives.empty() ? link.commands.front() : archives[0].commands.front();
    check(first.size() == 4 && first[2] == join_path(library_tree, "src/objs.a"), "objects archive per folder");
    bool apart = archives.size() == 3 && archives[1].commands.size() == 1 && archives[2].commands.size() == 1 &&
                 archives[1].commands[0][3].ends_with("sketch/Lamp.ino.cpp.o") &&
                 archives[2].commands[0][3].ends_with("sketch/src/nested.cpp.o");
    check(apart, "a folder's archive takes only that folder's objects, not those of the folders inside it");

    const std::vector<std::string>& words = link.commands.back();
    std::vector<std::string> middle(words.begin() + 4, words.end() - 1);
    std::vector<std::string> expected = { "-Wl,--whole-archive", join_path(BUILD_TREE, "libraries/Dots/Dots.a"),
                                          join_path(library_tree, "src/objs.a"), join_path(BUILD_TREE, "sketch/objs.a"),
                                          join_path(BUILD_TREE, "sketch/src/objs.a"), "-Wl,--no-whole-archive" };
    check(middle == expected, "the link takes the kept archives, then each folder's, whole");
}

// Under a zone with daylight saving time, the zone value is its standard offset and the daylight value the rest
static void check_time_values() {
    std::string previous = getenv("TZ") ? getenv("TZ") : "";
    setenv("TZ", "Europe/Berlin", 1);
    tzset();

    std::vector<installed_core> cores;
    property_table properties;
    sketch lamp;
    if (load_lamp(cores, properties, lamp)) {
        time_t now = time(nullptr);
        tm local = {};
        localtime_r(&now, &local);
        check(properties.get("extra.time.zone") == "3600", "the zone is Berlin's standard offset");
        check(properties.get("extra.time.dst") == std::to_string(local.tm_gmtoff - 3600),
              "daylight saving time adds the rest of the current offset");
    }

    if (previous.empty()) {
        unsetenv("TZ");
    } else {
        setenv("TZ", previous.c_str(), 1);
    }

    tzset();
}

static void check_objcopy_and_hooks() {
    std::vector<installed_core> cores;
    property_table properties;
    sketch lamp;
    if (!load_lamp(cores, properties, lamp)) {
        return;
    }

    std::vector<build_step> steps;
    std::string error;
    properties.set("recipe.objcopy.bin.pattern", "");
    properties.set("recipe.objcopy.pattern", "cp a b");
    check(plan_objcopy(properties, steps, error) && steps.size() == 1 && steps[0].recipe == "recipe.objcopy.pattern",
          "a bare objcopy recipe runs when no other holds a value, as findRecipes takes it");

    steps.clear();
    properties.set("recipe.objcopy.bin.pattern", "cp a c");
    check(plan_objcopy(properties, steps, error) && steps.size() == 1 &&
              steps[0].recipe == "recipe.objcopy.bin.pattern",
          "a bare objcopy recipe gives way to one that holds a value");

    check(runs_inside_mcufw({ "mcufw", "hook", "write", "/b/f" }), "a mcufw hook command runs inside mcufw");
    check(!runs_inside_mcufw({ "mcufw" }) && !runs_inside_mcufw({ "/bin/mcufw", "hook" }) &&
              !runs_inside_mcufw({ "mcufw", "build" }),
          "any other command runs as a child");
}

// arduino-cli's long-command rule measures the expanded recipe, quotes included, and rewrites the words naming files
// inside the build tree
static void check_long_commands() {
    char root_template[] = "/tmp/mcufw-long-XXXXXX";
    std::string tree = absolute(mkdtemp(root_template));
    std::string object = join_path(tree, "sketch/a.o");
    std::string error;
    check(make_directories(join_path(tree, "sketch"), error) && write_new_file(object, "", error), error);

    std::vector<std::string> original = { "tool", object, join_path(tree, "sketch/missing.o"),
                                          tree + "/sketch/../sketch/a.o", tree, "/elsewhere/b.o" };
    std::vector<std::string> words = original;
    std::string working_directory;
    shorten_long_command(tree, 30000, words, working_directory);
    check(words == original && working_directory.empty(), "a command of exactly 30,000 characters runs as it is");

    shorten_long_command(tree, 30001, words, working_directory);
    std::vector<std::string> expected = { "tool", "sketch/a.o", join_path(tree, "sketch/missing.o"),
                                          tree + "/sketch/../sketch/a.o", ".", "/elsewhere/b.o" };
    check(working_directory == tree && words == expected,
          "past 30,000 the command runs in the build tree with its existing files there named relatively");

    std::vector<installed_core> cores;
    property_table properties;
    sketch lamp;
    if (load_lamp(cores, properties, lamp)) {
        std::vector<build_step> hooks;
        properties.set("recipe.hooks.postbuild.1.pattern", "tool \"" + std::string(30001 - 7, 'a') + "\"");
        check(plan_hooks(properties, "postbuild", hooks, error) && hooks.size() == 1 &&
                  command_text_length(hooks[0], 0) == 30001 && joined(hooks[0].commands[0]).size() == 29999,
              "a recipe's quotes count toward its text, as arduino-cli measures it");

        build_step otherwise = { step_kind::hook, "recipe.hooks.prebuild.3.pattern", { { "mcufw", "hook", "write" } },
                                 "", "", {} };
        check(command_text_length(otherwise, 0) == 16, "a command no recipe expands measures its words joined");
    }

    remove_tree(tree, error);
}

int main() {
    check_arduino_commands();
    check_group_levels();
    check_warnings_and_names();
    check_words();
    check_long_link();

    check_time_values();
    check_objcopy_and_hooks();
    check_long_commands();

    if (g_failures != 0) {
        printf("build_plan: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("build_plan: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
