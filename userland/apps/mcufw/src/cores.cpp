#include "cores.hpp"

#include "files.hpp"
#include "versions.hpp"

#include <algorithm>

constexpr std::string_view OVERRIDE_SUFFIX = ".txt";

// The menus the board's lines use among those boards.txt declares, in declaration order. A choice is a first-level key
// of the menu's subtree that holds a label, so a key labeled only for another system is no choice.
static std::vector<board_menu> read_menus(const property_table& board_properties,
                                          const property_table& declared_menus) {
    std::vector<std::pair<std::string, property_table>> offered = board_properties.subtree("menu").split_first_level();
    std::vector<board_menu> menus;
    for (const std::string& menu_id : declared_menus.first_level_keys()) {
        for (const auto& [offered_id, choice_lines] : offered) {
            if (offered_id != menu_id) {
                continue;
            }

            board_menu menu{ menu_id, declared_menus.get(menu_id), {} };
            for (const std::string& choice_id : choice_lines.first_level_keys()) {
                if (const std::string* label = choice_lines.find(choice_id)) {
                    menu.choices.push_back({ choice_id, *label });
                }
            }

            menus.push_back(std::move(menu));
        }
    }

    return menus;
}

static bool read_install_record(const std::string& path, std::vector<tool_dependency>& tools, std::string& error) {
    property_table record;
    if (!load_properties(path, record, error)) {
        return false;
    }

    property_table entries = record.subtree("tool");
    for (const std::string& index : entries.first_level_keys()) {
        property_table fields = entries.subtree(index);
        tool_dependency tool{ fields.get("packager"), fields.get("name"), fields.get("version") };
        for (const char* field : { "packager", "name", "version" }) {
            if (fields.get(field).empty()) {
                error = path + " names tool " + index + " without its " + field;
                return false;
            }
        }

        tools.push_back(tool);
    }

    return true;
}

static bool load_optional_properties(const std::string& path, property_table& table, std::string& error) {
    return !is_file(path) || load_properties(path, table, error);
}

static bool read_boards(const std::string& directory, std::vector<board_definition>& boards, std::string& error) {
    property_table boards_file;
    if (!load_properties(join_path(directory, "boards.txt"), boards_file, error)) {
        return false;
    }

    if (!load_optional_properties(join_path(directory, "boards.local.txt"), boards_file, error)) {
        return false;
    }

    std::vector<std::pair<std::string, property_table>> tables = boards_file.split_first_level();
    property_table declared_menus;
    for (const auto& [id, table] : tables) {
        if (id == "menu") {
            declared_menus = table;
        }
    }

    for (auto& [id, table] : tables) {
        if (id == "menu") {
            continue;
        }

        board_definition board;
        board.id = id;
        board.name = table.get("name");
        board.hidden = table.get("hide") == "true";
        board.menus = read_menus(table, declared_menus);

        table.set("_id", id);
        board.properties = std::move(table);
        boards.push_back(std::move(board));
    }

    return true;
}

static bool load_core(const std::string& packager, const std::string& architecture, const std::string& version,
                      const std::string& directory, installed_core& core, std::string& error) {
    core = { packager, architecture, version, directory, {}, {}, {} };
    if (!load_properties(join_path(directory, "platform.txt"), core.platform, error)) {
        return false;
    }

    if (!load_optional_properties(join_path(directory, "platform.local.txt"), core.platform, error)) {
        return false;
    }

    if (!read_boards(directory, core.boards, error)) {
        return false;
    }

    std::string record = join_path(directory, INSTALL_RECORD_NAME);

    return !is_file(record) || read_install_record(record, core.tools, error);
}

bool load_installed_cores(const directory_layout& layout, std::vector<installed_core>& cores, std::string& error) {
    cores.clear();
    if (!is_directory(layout.hardware)) {
        return true;
    }

    std::vector<std::string> packagers;
    if (!list_subdirectories(layout.hardware, packagers, error)) {
        return false;
    }

    for (const std::string& packager : packagers) {
        std::string packager_directory = join_path(layout.hardware, packager);
        std::vector<std::string> architectures;
        if (!list_subdirectories(packager_directory, architectures, error)) {
            return false;
        }

        for (const std::string& architecture : architectures) {
            std::string architecture_directory = join_path(packager_directory, architecture);
            std::vector<std::string> versions;
            if (!list_subdirectories(architecture_directory, versions, error)) {
                return false;
            }

            if (versions.size() > 1) {
                error = architecture_directory + " holds more than one version of " + packager + ":" + architecture;
                return false;
            }

            if (versions.empty()) {
                continue;
            }

            installed_core core;
            if (!load_core(packager, architecture, versions[0], join_path(architecture_directory, versions[0]),
                           core, error)) {
                return false;
            }

            cores.push_back(std::move(core));
        }
    }

    return true;
}

bool write_install_record(const std::string& directory, const std::vector<tool_dependency>& tools, std::string& error) {
    std::string text;
    for (size_t index = 0; index < tools.size(); index++) {
        std::string prefix = "tool." + std::to_string(index) + ".";
        text += prefix + "packager=" + tools[index].packager + "\n";
        text += prefix + "name=" + tools[index].name + "\n";
        text += prefix + "version=" + tools[index].version + "\n";
    }

    return write_new_file(join_path(directory, INSTALL_RECORD_NAME), text, error);
}

bool overridden_versions(const directory_layout& layout, const std::string& packager, const std::string& architecture,
                         std::vector<std::string>& versions, std::string& error) {
    versions.clear();
    std::string directory = join_path(join_path(layout.overrides, packager), architecture);
    if (!is_directory(layout.overrides) || !is_directory(directory)) {
        return true;
    }

    std::vector<std::string> names;
    if (!list_directory(directory, names, error)) {
        return false;
    }

    for (const std::string& name : names) {
        if (name.size() > OVERRIDE_SUFFIX.size() && name.ends_with(OVERRIDE_SUFFIX)) {
            versions.push_back(name.substr(0, name.size() - OVERRIDE_SUFFIX.size()));
        }
    }

    std::sort(versions.begin(), versions.end(), [](const std::string& a, const std::string& b) {
        return compare_versions(a, b) > 0;
    });

    return true;
}
