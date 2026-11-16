/* cores, tools, boards and requirements, run on the build machine against fixtures/: cores, menus as read on Linux,
 * choosing boards, the layered properties, what a build is missing, and what `mcufw boards` lists and describes */

#include "boards.hpp"
#include "commands.hpp"
#include "cores.hpp"
#include "files.hpp"
#include "recipes.hpp"
#include "requirements.hpp"
#include "tools.hpp"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

constexpr const char* NATIVE_CORE = "fixtures/hardware/stellux/native/1.0.0";
constexpr const char* STATE_FOLDER = "../build/host/boards-state";
constexpr const char* ALPHA_SDK = "fixtures/tools/stellux/alpha-sdk/2.0";
constexpr const char* NATIVE_GCC = "fixtures/program-tools/stellux/native-gcc/1.0";

/* A board request that selection refuses, with the reason it gives */
struct refusal {
    const char* text;
    const char* error;
};

/* What one run of `mcufw boards` returned and wrote */
struct command_output {
    int         status;
    std::string out;
    std::string err;
};

/* Every board of the fixture cores that a listing shows, in its order */
constexpr const char* FULL_LISTING =
    "board\tstellux:native:beta\tstellux:beta-sdk 2.0\tBeta Board\n"
    "board\tstellux:native:cross\txtensa-cross-g++\tCross Board\n"
    "board\tstellux:native:dotless\t-\tDotless Menu Board\n"
    "board\tstellux:native:esp32da\t-\tESP32-WROOM-DA Module\n"
    "board\tstellux:native:gamma\tgamma-sdk\tGamma Board\n"
    "board\tstellux:native:hideempty\t-\tHide Empty Board\n"
    "board\tstellux:native:hidefalse\t-\tHide False Board\n"
    "board\tstellux:native:hideupper\t-\tHide Upper Board\n"
    "board\tstellux:native:menus\t-\tBoard With Menus\n"
    "board\tstellux:native:plain\t-\tPlain Board\n"
    "board\tstellux:native:twice\t-\tTwice Named Tool Board\n"
    "board\tstellux:native:versioned\tstellux:beta-sdk 2.0\tVersioned Board\n"
    "board\tstellux:other:plain\t-\tOther Plain Board\n";

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

static std::string read_back(FILE* file) {
    std::string text;
    char chunk[4096];
    rewind(file);
    while (size_t count = fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, count);
    }

    fclose(file);

    return text;
}

static directory_layout hardware_layout(const std::string& hardware) {
    directory_layout layout;
    layout.hardware = hardware;
    return layout;
}

static directory_layout fixture_layout() {
    directory_layout layout = hardware_layout("fixtures/hardware");
    layout.data_tools = "fixtures/tools";
    layout.program_tools = "fixtures/program-tools";
    layout.state = STATE_FOLDER;
    return layout;
}

static command_output run_command(const directory_layout& layout, const std::string& word, bool events) {
    arguments parsed;
    parsed.events = events;
    if (!word.empty()) {
        parsed.operands.push_back(word);
    }

    FILE* out = tmpfile();
    FILE* err = tmpfile();
    reporter report(events, out, err);
    int status = run_boards(parsed, layout, report);

    return { status, read_back(out), read_back(err) };
}

static const board_definition* find_definition(const installed_core& core, const std::string& id) {
    for (const board_definition& definition : core.boards) {
        if (definition.id == id) {
            return &definition;
        }
    }

    return nullptr;
}

static std::vector<std::string> choice_ids(const board_menu& menu) {
    std::vector<std::string> ids;
    for (const menu_choice& choice : menu.choices) {
        ids.push_back(choice.id);
    }

    return ids;
}

static bool select(const std::vector<installed_core>& cores, const std::string& text, selected_board& board,
                   std::string& error) {
    board_request request;
    return parse_board_request(text, request, error) && select_board(cores, request, board, error);
}

static board_requirements requirements_of(const std::vector<installed_core>& cores, const std::string& text,
                                          const directory_layout& layout) {
    selected_board board;
    std::string error;
    check(select(cores, text, board, error), "the board is chosen");
    return check_requirements(board, locate_tools(board.core->tools, layout));
}

static void check_cores(const std::vector<installed_core>& cores) {
    check(cores.size() == 2, "both cores load");
    const installed_core& native = cores[0];
    check(native.packager == "stellux" && native.architecture == "native" && native.version == "1.0.0",
          "cores come sorted, each with its version");
    check(native.directory == NATIVE_CORE && cores[1].architecture == "other", "a core knows its folder");

    check(native.platform.get("name") == "Stellux Native", "platform.txt loads");
    check(native.platform.get("build.extra_flags") == "-DSTELLUX_LOCAL", "platform.local.txt overrides platform.txt");

    std::vector<std::string> ids;
    for (const board_definition& definition : native.boards) {
        ids.push_back(definition.id);
    }

    std::vector<std::string> expected = {
        "plain",     "menus",     "beta",     "cross",            "gamma",  "versioned",  "twice",
        "hidden",    "hideupper", "hideempty", "hidefalse",        "borrowed", "borrowed_variant", "ownref",
        "undeclared", "nolabel",  "dotless",  "esp32da",
    };

    check(ids == expected, "boards come in boards.txt order, without the menu declarations");

    const board_definition* plain = find_definition(native, "plain");
    check(plain && plain->name == "Plain Board", "a board has its name");
    check(plain && plain->properties.get("build.variant") == "plain_local", "boards.local.txt overrides boards.txt");
    check(plain && plain->properties.get("_id") == "plain", "a board's _id is its ID");
    check(plain && !plain->hidden && find_definition(native, "hidden")->hidden, "hide=true hides a board");
    for (const char* id : { "hideupper", "hideempty", "hidefalse" }) {
        check(!find_definition(native, id)->hidden, "a hide value other than true leaves a board shown");
    }

    check(native.tools.size() == 3 && native.tools[1].packager == "stellux", "the install record names the tools");
    check(native.tools[1].name == "alpha-sdk" && native.tools[1].version == "2.0",
          "the install record's tools keep its order");
    check(cores[1].tools.empty(), "a core without a record depends on no tool");
}

static void check_menus(const installed_core& native) {
    const board_definition* menus = find_definition(native, "menus");
    check(menus && menus->menus.size() == 2, "a board offers only the menus it uses");
    check(menus->menus[0].id == "Speed" && menus->menus[0].label == "CPU Speed", "menus come in declared order");
    check(menus->menus[1].id == "Debug" && menus->menus[1].label == "Debug Level", "menus carry their declared labels");

    const board_menu& speed = menus->menus[0];
    check(choice_ids(speed) == std::vector<std::string>({ "slow", "turbo" }), "a choice labeled for Windows is none");
    check(speed.choices[1].label == "Turbo", "a choice labeled for Linux takes the label");

    const board_definition* esp32da = find_definition(native, "esp32da");
    std::vector<std::string> menu_ids;
    for (const board_menu& menu : esp32da->menus) {
        menu_ids.push_back(menu.id);
    }

    check(menu_ids == std::vector<std::string>({ "UploadSpeed", "FlashSize", "PartitionScheme" }),
          "esp32da's menus follow the declarations, not its lines");
    check(choice_ids(esp32da->menus[0]) == std::vector<std::string>({ "921600", "115200", "230400", "460800" }),
          "esp32da has four upload speeds as read on Linux");

    const board_definition* dotless = find_definition(native, "dotless");
    check(dotless && dotless->menus.empty(), "a menu line naming no choice is no menu");
}

static void check_loading_failures(const std::string& root) {
    std::vector<installed_core> cores;
    std::string error;
    directory_layout missing = hardware_layout(join_path(root, "missing"));
    check(load_installed_cores(missing, cores, error) && cores.empty(), "no hardware folder means no cores");

    std::string architecture = join_path(root, "hardware/stellux/twice");
    std::string make_versions = "mkdir -p " + architecture + "/1.0.0 " + architecture + "/2.0.0";
    check(system(make_versions.c_str()) == 0, "the versions are made");

    directory_layout twice = hardware_layout(join_path(root, "hardware"));
    twice.state = STATE_FOLDER;
    check(!load_installed_cores(twice, cores, error), "two versions of one core fail");
    check(error == architecture + " holds more than one version of stellux:twice", "the failure names the core");

    std::string core = join_path(root, "records/stellux/partial/1.0.0");
    std::string make_core = "mkdir -p " + core;
    check(system(make_core.c_str()) == 0, "the core is made");
    write_text(join_path(core, "platform.txt"), "name=Partial\n");
    write_text(join_path(core, "boards.txt"), "");
    write_text(join_path(core, "installed.txt"), "tool.0.packager=stellux\ntool.0.name=sdk\n");

    directory_layout partial = hardware_layout(join_path(root, "records"));
    check(!load_installed_cores(partial, cores, error), "a record naming a tool without its version fails");
    check(error == join_path(core, "installed.txt") + " names tool 0 without its version",
          "the failure names the record, the tool and its missing field");

    command_output listing = run_command(twice, "", true);
    check(listing.status == 1 && listing.out == "failed\tcore\t" + architecture +
                                                    " holds more than one version of stellux:twice\n",
          "a core that does not load fails the listing");
}

static void check_selection(const std::vector<installed_core>& cores) {
    selected_board board;
    std::string error;
    check(select(cores, "menus", board, error) && board.core == &cores[0], "a unique ID chooses its board");
    check(board.options.size() == 2 && board.options[0].choice_id == "slow", "the first labeled choice is the default");
    check(board.given_options.empty() && requested_fqbn(board) == "stellux:native:menus", "no option was given");
    check(full_fqbn(board) == "stellux:native:menus:Speed=slow,Debug=off", "the full FQBN names every default");

    check(select(cores, "stellux:native:menus:Debug=on", board, error), "an FQBN with an option chooses its board");
    check(board.options[0].menu_id == "Speed" && board.options[1].choice_id == "on", "defaults first, given last");
    check(requested_fqbn(board) == "stellux:native:menus:Debug=on", "the requested FQBN keeps what was given");
    check(full_fqbn(board) == "stellux:native:menus:Speed=slow,Debug=on", "the full FQBN follows menu order");

    check(select(cores, "menus:Speed=turbo", board, error), "an ID with an option chooses its board");
    check(board.options.size() == 2 && board.options[0].menu_id == "Debug" && board.options[1].choice_id == "turbo",
          "a given option follows the defaults of the other menus");
    check(full_fqbn(board) == "stellux:native:menus:Speed=turbo,Debug=off",
          "the full FQBN puts a given option back in menu order");

    check(select(cores, "dotless", board, error) && board.options.empty(), "a menu line naming no choice is ignored");

    check(select(cores, "stellux:other:plain", board, error) && board.core == &cores[1], "an FQBN picks the core");
    check(!select(cores, "plain", board, error), "an ID two cores define fails");
    check(error == "board plain is stellux:native:plain and stellux:other:plain, so name it by its FQBN",
          "the failure names both boards");
}

static void check_selection_failures(const std::vector<installed_core>& cores) {
    const refusal refusals[] = {
        { "nosuch", "no installed core defines board nosuch, so find one with mcufw boards" },
        { "stellux:native:nosuch", "stellux:native defines no board nosuch" },
        { "stellux:absent:plain", "stellux:absent is not installed" },
        { "menus:Color=red", "stellux:native:menus has no menu Color" },
        { "menus:Speed=fast", "menu Speed of stellux:native:menus has no choice fast" },
        { "borrowed", "stellux:native:borrowed takes build.core arduino:arduino from packager arduino, which mcufw "
                      "cannot build with" },
        { "borrowed_variant", "stellux:native:borrowed_variant takes build.variant arduino:standard from packager "
                              "arduino, which mcufw cannot build with" },
        { "ownref", "stellux:native:ownref takes build.core stellux:native from packager stellux, which mcufw cannot "
                    "build with" },
        { "undeclared", "stellux:native:undeclared uses menu Color, which boards.txt does not declare" },
        { "nolabel", "menu Debug of stellux:native:nolabel has no labeled choice" },
    };

    for (const refusal& expected : refusals) {
        selected_board board;
        std::string error;
        check(!select(cores, expected.text, board, error) && error == expected.error, expected.text);
    }
}

static void check_listing() {
    command_output all = run_command(fixture_layout(), "", true);
    check(all.status == 0 && all.out == FULL_LISTING, "a listing leaves out hidden and refused boards, by FQBN");
    check(all.err.empty(), "a listing writes nothing on stderr");

    command_output by_name = run_command(fixture_layout(), "WROOM", true);
    check(by_name.out == "board\tstellux:native:esp32da\t-\tESP32-WROOM-DA Module\n",
          "a word matches a name in any case");

    command_output by_fqbn = run_command(fixture_layout(), "Native:Hide", true);
    std::string expected = "board\tstellux:native:hideempty\t-\tHide Empty Board\n"
                           "board\tstellux:native:hidefalse\t-\tHide False Board\n"
                           "board\tstellux:native:hideupper\t-\tHide Upper Board\n";
    check(by_fqbn.out == expected, "a word matches an FQBN in any case");

    command_output shared_id = run_command(fixture_layout(), "plain", true);
    expected = "board\tstellux:native:plain\t-\tPlain Board\n"
               "board\tstellux:other:plain\t-\tOther Plain Board\n";
    check(shared_id.status == 0 && shared_id.out == expected, "an ID two cores define lists both boards");

    command_output person = run_command(fixture_layout(), "nothing-matches", false);
    check(person.out == "No installed board matches nothing-matches.\n", "a person learns that nothing matched");
}

static void check_descriptions() {
    command_output hidden = run_command(fixture_layout(), "stellux:native:hidden", true);
    check(hidden.status == 0 && hidden.out == "board\tstellux:native:hidden\t-\tHidden Board\n",
          "a hidden board named by its FQBN is described");

    command_output by_id = run_command(fixture_layout(), "menus", true);
    check(by_id.out.starts_with("board\tstellux:native:menus:Speed=slow,Debug=off\t-\tBoard With Menus\n"),
          "a unique ID is described rather than listed");

    command_output refused = run_command(fixture_layout(), "borrowed", true);
    check(refused.status == 1 && refused.out == "failed\tboard\tstellux:native:borrowed takes build.core "
                                                "arduino:arduino from packager arduino, which mcufw cannot build "
                                                "with\n",
          "a refused board named exactly gives the reason");

    command_output with_option = run_command(fixture_layout(), "menus:Speed=turbo", true);
    check(with_option.status == 0 &&
              with_option.out.starts_with("board\tstellux:native:menus:Speed=turbo,Debug=off\t-\tBoard With Menus\n"),
          "a word with an option describes the board with it");

    command_output person = run_command(fixture_layout(), "menus:Speed=turbo", false);
    check(person.out.starts_with("stellux:native:menus:Speed=turbo   Board With Menus, from Stellux Native 1.0.0\n"),
          "a person sees the board as requested");

    command_output bad_choice = run_command(fixture_layout(), "menus:Color=red", true);
    check(bad_choice.status == 1 && bad_choice.out == "failed\tboard\tstellux:native:menus has no menu Color\n",
          "a word with an option the board lacks fails");

    command_output bad_option = run_command(fixture_layout(), "menus:=red", true);
    check(bad_option.status == 1 && bad_option.out == "failed\tboard\t=red is not a menu=choice option\n",
          "a word with a malformed option fails");
}

static void check_properties(const std::vector<installed_core>& cores, const directory_layout& layout) {
    selected_board board;
    std::string error;
    check(select(cores, "menus:Speed=turbo", board, error), "the board is chosen");

    property_table properties = board_properties(board, locate_tools(board.core->tools, layout));
    check(properties.get("build.speed") == "turbo" && properties.get("build.debug") == "0",
          "a given option merges after the other menus' defaults");
    check(properties.get("build.fqbn") == "stellux:native:menus:Speed=turbo", "build.fqbn holds the request");
    check(properties.get("build.arch") == "NATIVE" && properties.get("build.board") == "MENUS_CUSTOM",
          "build.arch is the architecture and the board's own build.board stays");
    check(properties.get("_id") == "menus", "_id is the board's ID");

    check(properties.get("runtime.platform.path") == NATIVE_CORE, "runtime.platform.path is the core");
    check(properties.get("runtime.hardware.path") == "fixtures/hardware/stellux/native",
          "runtime.hardware.path is the core's parent");

    check(properties.get("build.board.platform.path") == NATIVE_CORE, "build.board.platform.path is the core");
    check(properties.get("build.core.platform.path") == NATIVE_CORE, "build.core.platform.path is the core");
    check(properties.get("build.core.path") == std::string(NATIVE_CORE) + "/cores/native",
          "build.core.path is the core's cores/<build.core>");
    check(properties.get("build.system.path") == std::string(NATIVE_CORE) + "/system",
          "build.system.path is the core's system folder");
    check(properties.get("build.variant.path").empty(), "no variant leaves build.variant.path empty");

    check(properties.get("runtime.tools.alpha-sdk.path") == ALPHA_SDK,
          "a data tool has its path, from the data folder though the program folder holds it too");
    check(properties.get("runtime.tools.alpha-sdk-2.0.path") == ALPHA_SDK, "a data tool has its versioned path");
    check(properties.get("runtime.tools.native-gcc.path") == NATIVE_GCC, "a program tool has its path");
    check(!properties.find("runtime.tools.beta-sdk.path"), "a missing tool has none");

    check(properties.get("runtime.os") == "linux", "runtime.os is linux");
    check(properties.get("runtime.ide.version") == "10607" && properties.get("ide_version") == "10607",
          "the IDE version is arduino-cli 1.6.0's");
    check(properties.get("runtime.ide.path") == "/bin", "runtime.ide.path is mcufw's folder");
    check(properties.get("software") == "ARDUINO", "software defaults to ARDUINO");
    check(properties.get("build.library_discovery_phase") == "0", "build.library_discovery_phase is 0");
    check(properties.get("tools.avrdude.path") == "{runtime.tools.avrdude.path}", "tools.avrdude.path is set");

    std::vector<std::string> words;
    check(recipe_words(properties, "recipe.cpp.o.pattern", words, error), "the compile recipe splits");
    check(words[0] == std::string(NATIVE_GCC) + "/bin/g++" && words[2] == "-DSTELLUX_LOCAL",
          "the recipe expands through every layer");

    check(select(cores, "stellux:native:plain", board, error), "the board is chosen");
    property_table plain = board_properties(board, {});
    check(plain.get("build.board") == "NATIVE_PLAIN", "build.board defaults to the architecture and ID");
    check(plain.get("build.variant.path") == std::string(NATIVE_CORE) + "/variants/plain_local", "a variant's path");

    check(select(cores, "menus", board, error), "the board is chosen");
    check(board_properties(board, {}).get("build.speed") == "debug", "defaults merge in menu order");

    check(select(cores, "stellux:other:plain", board, error), "the board is chosen");
    property_table other = board_properties(board, {});
    check(other.get("runtime.ide.path") == "/opt/arduino", "a core's own runtime.ide.path stays");
    check(other.get("software") == "STELLUX", "a core's own software stays");
}

static void check_install_records(const std::string& root) {
    std::string core = join_path(root, "recorded/stellux/native/1.0.0");
    std::string make_core = "mkdir -p " + core;
    check(system(make_core.c_str()) == 0, "the core is made");
    write_text(join_path(core, "platform.txt"), "name=Recorded\n");
    write_text(join_path(core, "boards.txt"), "");

    std::vector<tool_dependency> tools = { { "esp32", "esp-x32", "2601" }, { "arduino", "dfu-util", "0.11.0" } };
    std::string error;
    check(write_install_record(core, tools, error), "the record is written");

    std::vector<installed_core> cores;
    directory_layout layout = hardware_layout(join_path(root, "recorded"));
    check(load_installed_cores(layout, cores, error) && cores.size() == 1 && cores[0].tools.size() == 2,
          "and reads back");
    check(cores.size() == 1 && cores[0].tools[1].packager == "arduino" && cores[0].tools[1].version == "0.11.0",
          "field for field, in order");
}

static void check_overrides(const std::string& root) {
    directory_layout layout;
    layout.overrides = join_path(root, "overrides");
    std::vector<std::string> versions;
    std::string error;
    check(overridden_versions(layout, "esp32", "esp32", versions, error) && versions.empty(), "no overrides at all");

    std::string folder = join_path(layout.overrides, "esp32/esp32");
    std::string make_folder = "mkdir -p " + folder;
    check(system(make_folder.c_str()) == 0, "the overrides are made");
    for (const char* name : { "3.3.2.txt", "3.3.12.txt", "notes.md", "3.3.11.txt" }) {
        write_text(join_path(folder, name), "");
    }

    check(overridden_versions(layout, "esp32", "esp32", versions, error), "the overrides read");
    check(versions == std::vector<std::string>({ "3.3.12", "3.3.11", "3.3.2" }), "newest first, only versions");
    check(overridden_versions(layout, "esp32", "other", versions, error) && versions.empty(), "none for another core");
}

static void check_data_tools(const directory_layout& layout) {
    std::vector<tool_dependency> tools;
    std::string error;
    check(list_data_tools(layout, tools, error) && tools.size() == 1, "the data tools folder lists its tools");
    check(tools.size() == 1 && tools[0].packager == "stellux" && tools[0].name == "alpha-sdk" &&
          tools[0].version == "2.0", "as packager, name and version");
}

static void check_tool_order() {
    std::vector<located_tool> tools = {
        { { "stellux", "sdk", "1.10" }, "/newer" },
        { { "stellux", "sdk", "1.9" }, "/older" },
    };

    property_table properties;
    set_tool_paths(tools, properties);
    check(properties.get("runtime.tools.sdk.path") == "/newer", "the newest version takes the short key");
    check(properties.get("runtime.tools.sdk-1.9.path") == "/older", "each version keeps its own key");

    std::vector<located_tool> packagers = {
        { { "zeta", "sdk", "1.0" }, "/zeta" },
        { { "alpha", "sdk", "2.0" }, "/alpha" },
    };

    property_table by_packager;
    set_tool_paths(packagers, by_packager);
    check(by_packager.get("runtime.tools.sdk.path") == "/zeta", "tools sort by packager before version");
}

static void check_what_boards_need(const std::vector<installed_core>& cores, const directory_layout& layout) {
    board_requirements plain = requirements_of(cores, "stellux:native:plain", layout);
    check(plain.tools.size() == 2 && plain.tools[0].name == "native-gcc", "the recipe names the compiler's tool");
    check(plain.tools[1].name == "alpha-sdk", "the recipe names the SDK it includes");
    check(describe_missing(plain).empty(), "plain needs nothing");

    board_requirements twice = requirements_of(cores, "twice", layout);
    check(twice.tools.size() == 1 && twice.tools[0].name == "native-gcc", "a tool named twice is listed once");

    board_requirements beta = requirements_of(cores, "beta", layout);
    check(describe_missing(beta) == "stellux:beta-sdk 2.0", "a data tool no folder holds is missing");

    board_requirements versioned = requirements_of(cores, "versioned", layout);
    check(describe_missing(versioned) == "stellux:beta-sdk 2.0", "a tool named with its version is that dependency");

    board_requirements cross = requirements_of(cores, "cross", layout);
    check(describe_missing(cross) == "xtensa-cross-g++", "a compiler missing from its tool is missing");

    board_requirements gamma = requirements_of(cores, "gamma", layout);
    check(describe_missing(gamma) == "gamma-sdk", "a tool the core does not depend on is missing without a version");

    directory_layout no_programs = layout;
    no_programs.program_tools = "fixtures/missing";
    board_requirements bare = requirements_of(cores, "stellux:native:plain", no_programs);
    check(describe_missing(bare) == "stellux:native-gcc 1.0", "a missing tool hides the program it would hold");
}

int main() {
    char root_template[] = "/tmp/mcufw-boards-XXXXXX";
    std::string root = mkdtemp(root_template);
    directory_layout layout = fixture_layout();

    std::vector<installed_core> cores;
    std::string error;
    check(load_installed_cores(layout, cores, error), "the fixture cores load");
    if (cores.size() == 2) {
        check_cores(cores);
        check_menus(cores[0]);
        check_selection(cores);
        check_selection_failures(cores);

        check_listing();
        check_descriptions();
        check_properties(cores, layout);
        check_what_boards_need(cores, layout);
    }

    check_loading_failures(root);
    check_install_records(root);
    check_overrides(root);
    check_data_tools(fixture_layout());
    check_tool_order();

    std::string cleanup = "rm -rf " + root;
    if (system(cleanup.c_str()) != 0) {
        printf("boards: could not delete %s\n", root.c_str());
    }

    if (g_failures != 0) {
        printf("boards: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("boards: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
