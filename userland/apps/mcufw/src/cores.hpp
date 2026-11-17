#ifndef MCUFW_CORES_HPP
#define MCUFW_CORES_HPP

// Installed cores (Arduino's platforms), one version each, in `<packager>/<architecture>/<version>/`. mcufw's install
// record there names the exact tools the core's index entry depends on, and a core without one depends on no tool.

#include "layout.hpp"
#include "properties.hpp"

#include <string>
#include <vector>

constexpr const char* INSTALL_RECORD_NAME = "installed.txt";

struct tool_dependency {
    std::string packager;
    std::string name;
    std::string version;
};

struct menu_choice {
    std::string id;
    std::string label;
};

struct board_menu {
    std::string              id;
    std::string              label;
    std::vector<menu_choice> choices;  // in boards.txt order, the first being the default
};

struct board_definition {
    std::string             id;
    std::string             name;
    bool                    hidden;      // boards.txt sets `hide` to true, so listings leave the board out
    property_table          properties;  // the board's lines, its ID removed from each key, then `_id` set to it
    std::vector<board_menu> menus;       // the menus it offers, in the order boards.txt declares menus
};

struct installed_core {
    std::string                   packager;
    std::string                   architecture;
    std::string                   version;
    std::string                   directory;
    property_table                platform;  // platform.txt, the image's overrides of it, then platform.local.txt
    std::vector<board_definition> boards;    // boards.txt with boards.local.txt over it, in boards.txt order
    std::vector<tool_dependency>  tools;     // from the install record, in its order

    // Set when the image has overrides for this core, but none for its version
    std::string newest_overridden_version;
};

// Loads every installed core, ordered by packager, then architecture. A missing hardware folder holds none. Fails on
// the first core that does not load, such as one holding two versions or a file that does not parse.
bool load_installed_cores(const directory_layout& layout, std::vector<installed_core>& cores, std::string& error);

// Writes the install record into `directory`, the core's staged folder, failing when the archive put one there
bool write_install_record(const std::string& directory, const std::vector<tool_dependency>& tools, std::string& error);

// The versions of core `packager:architecture` that the image has overrides for, newest first. Fails when the core's
// folder of overrides exists but cannot be listed.
bool overridden_versions(const directory_layout& layout, const std::string& packager, const std::string& architecture,
                         std::vector<std::string>& versions, std::string& error);

#endif // MCUFW_CORES_HPP
