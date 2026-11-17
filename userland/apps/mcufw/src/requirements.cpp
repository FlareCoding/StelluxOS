#include "requirements.hpp"

#include "files.hpp"
#include "recipes.hpp"

#include <string_view>

constexpr const char* COMPILE_RECIPE = "recipe.cpp.o.pattern";
constexpr std::string_view PATTERN_SUFFIX = ".pattern";
constexpr std::string_view TOOL_PATH_START = "{runtime.tools.";
constexpr std::string_view TOOL_PATH_END = ".path";

// The tool names in the `{runtime.tools.<name>.path}` placeholders that expansion left in `text`
static std::vector<std::string> named_tools(const std::string& text) {
    std::vector<std::string> names;
    size_t start = text.find(TOOL_PATH_START);
    while (start != std::string::npos) {
        size_t name_start = start + TOOL_PATH_START.size();
        size_t close = text.find('}', name_start);
        if (close == std::string::npos) {
            break;
        }

        std::string_view inner = std::string_view(text).substr(name_start, close - name_start);
        bool is_tool_path = inner.ends_with(TOOL_PATH_END) && inner.find('{') == std::string_view::npos;
        if (is_tool_path) {
            names.emplace_back(inner.substr(0, inner.size() - TOOL_PATH_END.size()));
        }

        start = text.find(TOOL_PATH_START, close);
    }

    return names;
}

static bool is_same_tool(const tool_dependency& a, const tool_dependency& b) {
    return a.packager == b.packager && a.name == b.name && a.version == b.version;
}

static void add_unique(std::vector<tool_dependency>& tools, const tool_dependency& tool) {
    for (const tool_dependency& listed : tools) {
        if (is_same_tool(listed, tool)) {
            return;
        }
    }

    tools.push_back(tool);
}

// A recipe names a dependency as `<name>` or `<name>-<version>`, the keys `set_tool_paths` sets
static bool is_named(const tool_dependency& tool, const std::string& name) {
    return tool.name == name || tool.name + "-" + tool.version == name;
}

// The upload tool a serial port uses, as arduino-cli picks it, with a bare `upload.tool` read as the default as on
// older platforms
static std::string serial_upload_tool(const property_table& properties) {
    for (const char* key : { "upload.tool.serial", "upload.tool.default", "upload.tool" }) {
        if (const std::string* tool = properties.find(key)) {
            return *tool;
        }
    }

    return {};
}

// The tools every recipe names, and the upload tool's pattern with that tool's own properties over the board's
static std::vector<std::string> tools_named(const property_table& properties) {
    std::vector<std::string> patterns;
    for (const std::string& key : properties.keys()) {
        if (key.starts_with("recipe.") && key.ends_with(PATTERN_SUFFIX)) {
            patterns.push_back(properties.expand(properties.get(key)));
        }
    }

    std::string tool = serial_upload_tool(properties);
    if (!tool.empty()) {
        property_table uploading = properties;
        uploading.merge(properties.subtree("tools." + tool));
        patterns.push_back(uploading.expand(uploading.get("upload.pattern")));
    }

    std::vector<std::string> names;
    for (const std::string& pattern : patterns) {
        std::vector<std::string> named = named_tools(pattern);
        names.insert(names.end(), named.begin(), named.end());
    }

    return names;
}

board_requirements check_requirements(const selected_board& board, const std::vector<located_tool>& tools) {
    board_requirements requirements;
    const installed_core& core = *board.core;
    if (!core.newest_overridden_version.empty()) {
        requirements.missing_overrides = core.packager + ":" + core.architecture + " " + core.newest_overridden_version;
    }

    property_table without_tools = board_properties(board, {});
    property_table with_tools = without_tools;
    set_tool_paths(tools, with_tools);

    for (const std::string& name : tools_named(without_tools)) {
        bool is_dependency = false;
        bool is_located = with_tools.find("runtime.tools." + name + ".path");
        for (const located_tool& tool : tools) {
            if (!is_named(tool.dependency, name)) {
                continue;
            }

            is_dependency = true;
            add_unique(requirements.tools, tool.dependency);
            if (!is_located) {
                add_unique(requirements.missing_tools, tool.dependency);
            }
        }

        if (!is_dependency) {
            add_unique(requirements.missing_tools, { "", name, "" });
        }
    }

    std::vector<std::string> words;
    std::string error;
    bool has_program = recipe_words(with_tools, COMPILE_RECIPE, words, error);
    bool is_resolved = has_program && words[0].find('{') == std::string::npos;
    if (is_resolved && find_program(words[0]).empty()) {
        requirements.missing_program = file_name(words[0]);
    }

    return requirements;
}

std::string describe_missing(const board_requirements& requirements) {
    std::string description = requirements.missing_program;
    if (!requirements.missing_overrides.empty()) {
        description = requirements.missing_overrides + (description.empty() ? "" : ", " + description);
    }

    for (const tool_dependency& tool : requirements.missing_tools) {
        if (!description.empty()) {
            description += ", ";
        }

        description += tool.packager.empty() ? tool.name : tool.packager + ":" + tool.name + " " + tool.version;
    }

    return description;
}
