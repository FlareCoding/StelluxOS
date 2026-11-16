#include "boards.hpp"

#include <cctype>

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
