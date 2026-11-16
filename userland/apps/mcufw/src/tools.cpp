#include "tools.hpp"

#include "files.hpp"
#include "versions.hpp"

#include <algorithm>

static std::string tool_directory(const std::string& root, const tool_dependency& tool) {
    return join_path(join_path(join_path(root, tool.packager), tool.name), tool.version);
}

std::vector<located_tool> locate_tools(const std::vector<tool_dependency>& tools, const directory_layout& layout) {
    std::vector<located_tool> located;
    for (const tool_dependency& tool : tools) {
        std::string data_directory = tool_directory(layout.data_tools, tool);
        std::string program_directory = tool_directory(layout.program_tools, tool);
        if (is_directory(data_directory)) {
            located.push_back({ tool, data_directory });
        } else if (is_directory(program_directory)) {
            located.push_back({ tool, program_directory });
        } else {
            located.push_back({ tool, {} });
        }
    }

    return located;
}

// arduino-cli's order of tool dependencies: by packager, then name, then version
static bool comes_before(const located_tool& a, const located_tool& b) {
    if (a.dependency.packager != b.dependency.packager) {
        return a.dependency.packager < b.dependency.packager;
    }

    if (a.dependency.name != b.dependency.name) {
        return a.dependency.name < b.dependency.name;
    }

    return compare_versions(a.dependency.version, b.dependency.version) < 0;
}

void set_tool_paths(const std::vector<located_tool>& tools, property_table& properties) {
    std::vector<located_tool> sorted = tools;
    std::stable_sort(sorted.begin(), sorted.end(), comes_before);
    for (const located_tool& tool : sorted) {
        if (tool.directory.empty()) {
            continue;
        }

        const tool_dependency& dependency = tool.dependency;
        properties.set("runtime.tools." + dependency.name + ".path", tool.directory);
        properties.set("runtime.tools." + dependency.name + "-" + dependency.version + ".path", tool.directory);
    }
}
