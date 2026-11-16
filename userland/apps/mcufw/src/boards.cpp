#include "boards.hpp"

#include "files.hpp"

#include <cctype>

// The `runtime.ide.version` and `ide_version` arduino-cli 1.6.0 gives every board
constexpr const char* ARDUINO_IDE_VERSION = "10607";

// The system whose keys properties read, which recipes test as `runtime.os`
constexpr std::string_view RUNTIME_OS = LINUX_KEY_PART.substr(1);

// arduino-cli names the folder holding its own program as `runtime.ide.path`, and mcufw is installed in /bin
constexpr const char* MCUFW_DIRECTORY = "/bin";

static std::string uppercased(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    return text;
}

static std::string platform_id(const installed_core& core) {
    return core.packager + ":" + core.architecture;
}

static std::string board_fqbn(const installed_core& core, const board_definition& definition) {
    return platform_id(core) + ":" + definition.id;
}

static const board_menu* find_menu(const board_definition& definition, const std::string& menu_id) {
    for (const board_menu& menu : definition.menus) {
        if (menu.id == menu_id) {
            return &menu;
        }
    }

    return nullptr;
}

static bool has_choice(const board_menu& menu, const std::string& choice_id) {
    for (const menu_choice& choice : menu.choices) {
        if (choice.id == choice_id) {
            return true;
        }
    }

    return false;
}

static bool find_board_by_id(const std::vector<installed_core>& cores, const std::string& board_id,
                             selected_board& board, std::string& error) {
    std::vector<std::string> fqbns;
    for (const installed_core& core : cores) {
        for (const board_definition& definition : core.boards) {
            if (definition.id == board_id) {
                board.core = &core;
                board.definition = &definition;
                fqbns.push_back(board_fqbn(core, definition));
            }
        }
    }

    if (fqbns.empty()) {
        error = "no installed core defines board " + board_id + ", so find one with mcufw boards";
        return false;
    }

    if (fqbns.size() > 1) {
        error = "board " + board_id + " is " + fqbns[0] + " and " + fqbns[1] + ", so name it by its FQBN";
        return false;
    }

    return true;
}

static bool find_board_by_fqbn(const std::vector<installed_core>& cores, const board_request& request,
                               selected_board& board, std::string& error) {
    for (const installed_core& core : cores) {
        if (core.packager != request.packager || core.architecture != request.architecture) {
            continue;
        }

        for (const board_definition& definition : core.boards) {
            if (definition.id == request.board_id) {
                board.core = &core;
                board.definition = &definition;
                return true;
            }
        }

        error = platform_id(core) + " defines no board " + request.board_id;
        return false;
    }

    error = request.packager + ":" + request.architecture + " is not installed";

    return false;
}

// The board's lines with its options merged over them, defaults first, as arduino-cli's `GetBuildProperties` has it
static property_table board_build_properties(const selected_board& board) {
    const board_definition& definition = *board.definition;

    property_table properties = definition.properties;
    properties.set("build.fqbn", requested_fqbn(board));
    properties.set("build.arch", uppercased(board.core->architecture));
    for (const option_setting& option : board.options) {
        properties.merge(definition.properties.subtree("menu." + option.menu_id + "." + option.choice_id));
    }

    return properties;
}

// A `build.core` or `build.variant` written `packager:name` takes the files of the core that packager installed, which
// mcufw does not look up, so such a board is refused even when the packager is its own
static bool check_core_references(const selected_board& board, std::string& error) {
    property_table properties = board_build_properties(board);
    for (const char* key : { "build.core", "build.variant" }) {
        std::string value = properties.expand(properties.get(key));
        size_t colon = value.find(':');
        if (colon != std::string::npos) {
            error = board_fqbn(*board.core, *board.definition) + " takes " + key + " " + value + " from packager " +
                    value.substr(0, colon) + ", which mcufw cannot build with";
            return false;
        }
    }

    return true;
}

// As in arduino-cli, a board using an undeclared menu or a menu with no labeled choice fails. arduino-cli also fails
// a menu whose first key has no label, where mcufw defaults to the first labeled choice, the one `mcufw boards` shows.
static bool check_menus(const selected_board& board, std::string& error) {
    const board_definition& definition = *board.definition;
    for (const auto& [menu_id, choice_lines] : definition.properties.subtree("menu").split_first_level()) {
        if (!find_menu(definition, menu_id)) {
            error = board_fqbn(*board.core, definition) + " uses menu " + menu_id +
                    ", which boards.txt does not declare";
            return false;
        }
    }

    for (const board_menu& menu : definition.menus) {
        if (menu.choices.empty()) {
            error = "menu " + menu.id + " of " + board_fqbn(*board.core, definition) + " has no labeled choice";
            return false;
        }
    }

    return true;
}

static bool check_options(const selected_board& board, const std::vector<option_setting>& options,
                          std::string& error) {
    const board_definition& definition = *board.definition;
    for (const option_setting& option : options) {
        const board_menu* menu = find_menu(definition, option.menu_id);
        if (!menu) {
            error = board_fqbn(*board.core, definition) + " has no menu " + option.menu_id;
            return false;
        }

        if (!has_choice(*menu, option.choice_id)) {
            error = "menu " + option.menu_id + " of " + board_fqbn(*board.core, definition) + " has no choice " +
                    option.choice_id;
            return false;
        }
    }

    return true;
}

bool select_board(const std::vector<installed_core>& cores, const board_request& request, selected_board& board,
                  std::string& error) {
    board = {};
    bool found = request.packager.empty() ? find_board_by_id(cores, request.board_id, board, error)
                                          : find_board_by_fqbn(cores, request, board, error);
    if (!found) {
        return false;
    }

    if (!check_menus(board, error) || !check_options(board, request.options, error)) {
        return false;
    }

    board.given_options = request.options;
    for (const board_menu& menu : board.definition->menus) {
        bool is_given = false;
        for (const option_setting& option : request.options) {
            is_given = is_given || option.menu_id == menu.id;
        }

        if (!is_given) {
            board.options.push_back({ menu.id, menu.choices.front().id });
        }
    }

    board.options.insert(board.options.end(), request.options.begin(), request.options.end());

    return check_core_references(board, error);
}

std::string requested_fqbn(const selected_board& board) {
    return format_fqbn(board.core->packager, board.core->architecture, board.definition->id, board.given_options);
}

std::string full_fqbn(const selected_board& board) {
    std::vector<option_setting> in_menu_order;
    for (const board_menu& menu : board.definition->menus) {
        for (const option_setting& option : board.options) {
            if (option.menu_id == menu.id) {
                in_menu_order.push_back(option);
            }
        }
    }

    return format_fqbn(board.core->packager, board.core->architecture, board.definition->id, in_menu_order);
}

property_table board_properties(const selected_board& board, const std::vector<located_tool>& tools) {
    const installed_core& core = *board.core;
    const board_definition& definition = *board.definition;

    property_table board_build = board_build_properties(board);
    std::string core_folder = board_build.expand(board_build.get("build.core"));
    std::string variant = board_build.expand(board_build.get("build.variant"));
    std::string variant_path = variant.empty() ? "" : join_path(join_path(core.directory, "variants"), variant);

    property_table properties = core.platform;
    properties.merge(board_build);
    properties.set("runtime.platform.path", core.directory);
    properties.set("runtime.hardware.path", parent_directory(core.directory));

    properties.set("build.board.platform.path", core.directory);
    properties.set("build.core.platform.path", core.directory);
    properties.set("build.core.path", join_path(join_path(core.directory, "cores"), core_folder));
    properties.set("build.system.path", join_path(core.directory, "system"));
    properties.set("build.variant.path", variant_path);
    set_tool_paths(tools, properties);

    if (!properties.find("runtime.ide.path")) {
        properties.set("runtime.ide.path", MCUFW_DIRECTORY);
    }

    properties.set("runtime.os", std::string(RUNTIME_OS));
    properties.set("build.library_discovery_phase", "0");
    if (properties.get("build.board").empty()) {
        properties.set("build.board", uppercased(core.architecture + "_" + definition.id));
    }

    // arduino-cli 1.6.0's `ResolveFQBN` sets these deprecated properties for every board
    properties.set("tools.avrdude.path", "{runtime.tools.avrdude.path}");
    properties.set("ide_version", ARDUINO_IDE_VERSION);
    properties.set("runtime.ide.version", ARDUINO_IDE_VERSION);
    if (!properties.find("software")) {
        properties.set("software", "ARDUINO");
    }

    return properties;
}
