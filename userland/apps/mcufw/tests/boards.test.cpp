/* cores and boards, run on the build machine against fixtures/: cores with their local files and install records,
 * menus as read on Linux, choosing a board with its options, and what `mcufw boards` lists and describes */

#include "boards.hpp"
#include "commands.hpp"
#include "cores.hpp"
#include "files.hpp"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

constexpr const char* NATIVE_CORE = "fixtures/hardware/stellux/native/1.0.0";

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
    "board\tstellux:native:dotless\tDotless Menu Board\n"
    "board\tstellux:native:esp32da\tESP32-WROOM-DA Module\n"
    "board\tstellux:native:hideempty\tHide Empty Board\n"
    "board\tstellux:native:hidefalse\tHide False Board\n"
    "board\tstellux:native:hideupper\tHide Upper Board\n"
    "board\tstellux:native:menus\tBoard With Menus\n"
    "board\tstellux:native:plain\tPlain Board\n"
    "board\tstellux:other:plain\tOther Plain Board\n";

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

static directory_layout fixture_layout() {
    return { "fixtures/hardware", "", "" };
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
        "plain",           "menus",  "hidden",     "hideupper", "hideempty", "hidefalse", "borrowed",
        "borrowed_variant", "ownref", "undeclared", "nolabel",   "dotless",   "esp32da",
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
    directory_layout missing = { join_path(root, "missing"), "", "" };
    check(load_installed_cores(missing, cores, error) && cores.empty(), "no hardware folder means no cores");

    std::string architecture = join_path(root, "hardware/stellux/twice");
    std::string make_versions = "mkdir -p " + architecture + "/1.0.0 " + architecture + "/2.0.0";
    check(system(make_versions.c_str()) == 0, "the versions are made");

    directory_layout twice = { join_path(root, "hardware"), "", "" };
    check(!load_installed_cores(twice, cores, error), "two versions of one core fail");
    check(error == architecture + " holds more than one version of stellux:twice", "the failure names the core");

    std::string core = join_path(root, "records/stellux/partial/1.0.0");
    std::string make_core = "mkdir -p " + core;
    check(system(make_core.c_str()) == 0, "the core is made");
    write_text(join_path(core, "platform.txt"), "name=Partial\n");
    write_text(join_path(core, "boards.txt"), "");
    write_text(join_path(core, "installed.txt"), "tool.0.packager=stellux\ntool.0.name=sdk\n");

    directory_layout partial = { join_path(root, "records"), "", "" };
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
    check(by_name.out == "board\tstellux:native:esp32da\tESP32-WROOM-DA Module\n", "a word matches a name in any case");

    command_output by_fqbn = run_command(fixture_layout(), "Native:Hide", true);
    std::string expected = "board\tstellux:native:hideempty\tHide Empty Board\n"
                           "board\tstellux:native:hidefalse\tHide False Board\n"
                           "board\tstellux:native:hideupper\tHide Upper Board\n";
    check(by_fqbn.out == expected, "a word matches an FQBN in any case");

    command_output shared_id = run_command(fixture_layout(), "plain", true);
    expected = "board\tstellux:native:plain\tPlain Board\n"
               "board\tstellux:other:plain\tOther Plain Board\n";
    check(shared_id.status == 0 && shared_id.out == expected, "an ID two cores define lists both boards");

    command_output person = run_command(fixture_layout(), "nothing-matches", false);
    check(person.out == "No installed board matches nothing-matches.\n", "a person learns that nothing matched");
}

static void check_descriptions() {
    command_output hidden = run_command(fixture_layout(), "stellux:native:hidden", true);
    check(hidden.status == 0 && hidden.out == "board\tstellux:native:hidden\tHidden Board\n",
          "a hidden board named by its FQBN is described");

    command_output by_id = run_command(fixture_layout(), "menus", true);
    check(by_id.out.starts_with("board\tstellux:native:menus:Speed=slow,Debug=off\tBoard With Menus\n"),
          "a unique ID is described rather than listed");

    command_output refused = run_command(fixture_layout(), "borrowed", true);
    check(refused.status == 1 && refused.out == "failed\tboard\tstellux:native:borrowed takes build.core "
                                                "arduino:arduino from packager arduino, which mcufw cannot build "
                                                "with\n",
          "a refused board named exactly gives the reason");

    command_output with_option = run_command(fixture_layout(), "menus:Speed=turbo", true);
    check(with_option.status == 0 &&
              with_option.out.starts_with("board\tstellux:native:menus:Speed=turbo,Debug=off\tBoard With Menus\n"),
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

int main() {
    char root_template[] = "/tmp/mcufw-boards-XXXXXX";
    std::string root = mkdtemp(root_template);

    std::vector<installed_core> cores;
    std::string error;
    check(load_installed_cores(fixture_layout(), cores, error), "the fixture cores load");
    if (cores.size() == 2) {
        check_cores(cores);
        check_menus(cores[0]);
        check_selection(cores);
        check_selection_failures(cores);
        check_listing();
        check_descriptions();
    }

    check_loading_failures(root);

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
