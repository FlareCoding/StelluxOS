#ifndef MCUFW_TOOLS_HPP
#define MCUFW_TOOLS_HPP

// Where a core's tools are installed. A data tool, the same download for every host, is in the data tools folder, and a
// program in the program tools folder, as its Stellux package's link. A version both hold comes from the data folder.

#include "cores.hpp"
#include "layout.hpp"
#include "properties.hpp"

#include <string>
#include <vector>

struct located_tool {
    tool_dependency dependency;
    std::string     directory;  // empty when neither folder holds this version
};

std::vector<located_tool> locate_tools(const std::vector<tool_dependency>& tools, const directory_layout& layout);

// Sets `runtime.tools.<name>.path` and `runtime.tools.<name>-<version>.path` for each located tool, sorted as
// arduino-cli's `ToolDependencies.Sort` sorts them, so the newest of two versions of one tool takes the shorter key
void set_tool_paths(const std::vector<located_tool>& tools, property_table& properties);

// The data tools in the data tools folder
bool list_data_tools(const directory_layout& layout, std::vector<tool_dependency>& tools, std::string& error);

#endif // MCUFW_TOOLS_HPP
