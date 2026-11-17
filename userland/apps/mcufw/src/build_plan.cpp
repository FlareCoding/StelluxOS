#include "build_plan.hpp"

#include "digest.hpp"
#include "files.hpp"
#include "recipes.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <ctime>

constexpr size_t BUILD_DIRECTORY_DIGITS = 8;

// arduino-cli links objects through an archive per folder once their quoted list passes this many characters
constexpr size_t LINKED_OBJECTS_MAX = 30000;

// arduino-cli runs a command whose expanded recipe passes this many characters in the build tree
constexpr size_t COMMAND_LENGTH_MAX = 30000;

constexpr const char* FOLDER_ARCHIVE = "objs.a";
constexpr const char* SKETCH_FOLDER = "sketch";
constexpr const char* SKETCH_SOURCE_FOLDER = "src";
constexpr const char* CORE_FOLDER = "core";
constexpr const char* NO_WARNINGS = "none";

// The order of arduino-cli's builder: `preprocess` and then `build`, less the steps later stages add
constexpr stage_step BUILD_STAGES[] = {
    { build_stage::hooks, "prebuild" },
    { build_stage::sketch_tree, "" },
    { build_stage::hooks, "sketch.prebuild" },
    { build_stage::sketch_compiles, "" },
    { build_stage::hooks, "sketch.postbuild" },
    { build_stage::hooks, "libraries.prebuild" },
    { build_stage::hooks, "libraries.postbuild" },
    { build_stage::hooks, "core.prebuild" },
    { build_stage::core_compiles, "" },
    { build_stage::core_archive, "" },
    { build_stage::hooks, "core.postbuild" },
    { build_stage::hooks, "linking.prelink" },
    { build_stage::link, "" },
    { build_stage::hooks, "linking.postlink" },
    { build_stage::hooks, "objcopy.preobjcopy" },
    { build_stage::objcopy, "" },
    { build_stage::hooks, "objcopy.postobjcopy" },
    { build_stage::hooks, "postbuild" },
    { build_stage::size, "" },
};

std::vector<stage_step> build_stages() {
    return { std::begin(BUILD_STAGES), std::end(BUILD_STAGES) };
}

std::string build_directory_for(const directory_layout& layout, const std::string& sketch_folder) {
    sha256_digest digest;
    digest.update(sketch_folder);
    std::string name = file_name(sketch_folder) + "-" + digest.hex().substr(0, BUILD_DIRECTORY_DIGITS);
    return join_path(layout.builds, name);
}

static long zone_offset_on_first_day(int year, int month) {
    tm date = {};
    date.tm_year = year;
    date.tm_mon = month;
    date.tm_mday = 1;
    date.tm_isdst = -1;
    time_t then = mktime(&date);

    tm local = {};
    localtime_r(&then, &local);
    return local.tm_gmtoff;
}

// arduino-cli's `extra.time` values in seconds: the time, the local time, the zone's offset without daylight saving
// time, and the offset daylight saving time adds at the build's time
static void set_time_values(property_table& properties) {
    time_t now = time(nullptr);
    tm local = {};
    localtime_r(&now, &local);
    long winter_offset = zone_offset_on_first_day(local.tm_year, 0);
    long summer_offset = zone_offset_on_first_day(local.tm_year, 6);
    long standard_offset = std::min(winter_offset, summer_offset);

    properties.set("extra.time.utc", std::to_string(now));
    properties.set("extra.time.local", std::to_string(now + local.tm_gmtoff));
    properties.set("extra.time.zone", std::to_string(standard_offset));
    properties.set("extra.time.dst", std::to_string(local.tm_gmtoff - standard_offset));
}

property_table build_properties(const property_table& board_properties, const sketch& sketch,
                                const std::string& build_directory) {
    property_table properties = board_properties;
    set_time_values(properties);

    properties.set("sketch_path", sketch.folder);
    properties.set("build.project_name", sketch.main_file);
    properties.set("build.source.path", sketch.folder);
    properties.set("build.path", build_directory);
    if (const std::string* release_flags = properties.find("compiler.optimization_flags.release")) {
        properties.set("compiler.optimization_flags", *release_flags);
    }

    return properties;
}

bool plan_hooks(const property_table& properties, std::string_view point, std::vector<build_step>& steps,
                std::string& error) {
    for (const std::string& key : recipe_keys(properties, "recipe.hooks." + std::string(point))) {
        std::vector<std::string> words;
        size_t length = 0;
        if (!recipe_words(properties, key, words, length, error)) {
            return false;
        }

        steps.push_back({ step_kind::hook, key, { words }, "", "", { length } });
    }

    return true;
}

static std::string includes_value(const std::vector<std::string>& include_directories) {
    std::string includes;
    for (const std::string& directory : include_directories) {
        includes += (includes.empty() ? "\"-I" : " \"-I") + directory + "\"";
    }

    return includes;
}

// The recipe compiling a source by its extension, `.cxx` and `.cc` falling back to C++'s when the core has none
static std::string compile_recipe(const property_table& properties, const std::string& source) {
    std::string name = file_name(source);
    size_t dot = name.rfind('.');
    std::string extension = dot == std::string::npos ? "" : name.substr(dot);
    std::string recipe = "recipe" + extension + ".o.pattern";
    bool compiles_as_cpp = extension == ".cxx" || extension == ".cc";

    return properties.find(recipe) == nullptr && compiles_as_cpp ? "recipe.cpp.o.pattern" : recipe;
}

bool plan_compiles(const property_table& properties, const std::vector<std::string>& sources,
                   const std::string& source_root, const std::string& object_directory,
                   const std::vector<std::string>& include_directories, std::string_view warnings,
                   std::vector<build_step>& steps, std::string& error) {
    property_table compiling = properties;
    compiling.set("compiler.warning_flags", properties.get("compiler.warning_flags." + std::string(warnings)));
    compiling.set("includes", includes_value(include_directories));

    for (const std::string& source : sources) {
        if (!source.starts_with(source_root + "/")) {
            error = source + " is not inside " + source_root;
            return false;
        }

        std::string object = join_path(object_directory, source.substr(source_root.size() + 1) + ".o");
        compiling.set("source_file", source);
        compiling.set("object_file", object);

        std::string recipe = compile_recipe(properties, source);
        std::vector<std::string> words;
        size_t length = 0;
        if (!recipe_words(compiling, recipe, words, length, error)) {
            return false;
        }

        steps.push_back({ step_kind::compile, recipe, { words }, source, object, { length } });
    }

    return true;
}

bool plan_archive(const property_table& properties, const std::vector<std::string>& objects,
                  const std::string& archive, build_step& step, std::string& error) {
    step = { step_kind::archive, "recipe.ar.pattern", {}, "", archive, {} };

    property_table archiving = properties;
    archiving.set("archive_file", file_name(archive));
    archiving.set("archive_file_path", archive);
    for (const std::string& object : objects) {
        archiving.set("object_file", object);

        std::vector<std::string> words;
        size_t length = 0;
        if (!recipe_words(archiving, step.recipe, words, length, error)) {
            return false;
        }

        step.commands.push_back(words);
        step.lengths.push_back(length);
    }

    return true;
}

static std::string quoted_list(const std::vector<std::string>& paths) {
    std::string list;
    for (const std::string& path : paths) {
        list += (list.empty() ? "\"" : " \"") + path + "\"";
    }

    return list;
}

static std::vector<std::string> path_parts(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) {
            slash = path.size();
        }

        if (slash > start) {
            parts.push_back(path.substr(start, slash - start));
        }

        start = slash + 1;
    }

    return parts;
}

// The absolute path `to` relative to the absolute directory `from`
static std::string relative_path(const std::string& from, const std::string& to) {
    std::vector<std::string> from_parts = path_parts(from);
    std::vector<std::string> to_parts = path_parts(to);
    size_t common = 0;
    while (common < from_parts.size() && common < to_parts.size() && from_parts[common] == to_parts[common]) {
        common++;
    }

    std::vector<std::string> parts(from_parts.size() - common, "..");
    parts.insert(parts.end(), to_parts.begin() + static_cast<ptrdiff_t>(common), to_parts.end());

    std::string relative;
    for (const std::string& part : parts) {
        relative += (relative.empty() ? "" : "/") + part;
    }

    return relative.empty() ? "." : relative;
}

// arduino-cli's long link: objects kept as archives stay apart, and the others go into an archive per folder, which
// the link then takes whole
static bool plan_folder_archives(const property_table& properties, const std::vector<std::string>& objects,
                                 std::vector<build_step>& steps, std::string& object_files, std::string& error) {
    std::vector<std::string> archives;
    std::vector<std::string> loose;
    for (const std::string& object : objects) {
        (object.ends_with(".a") ? archives : loose).push_back(object);
    }

    std::vector<std::string> folder_archives;
    for (const std::string& object : loose) {
        std::string archive = join_path(parent_directory(object), FOLDER_ARCHIVE);
        if (std::find(folder_archives.begin(), folder_archives.end(), archive) == folder_archives.end()) {
            folder_archives.push_back(archive);
        }
    }

    for (const std::string& archive : folder_archives) {
        std::vector<std::string> members;
        for (const std::string& object : loose) {
            if (parent_directory(object) == parent_directory(archive)) {
                members.push_back(object);
            }
        }

        build_step archiving;
        if (!plan_archive(properties, members, archive, archiving, error)) {
            return false;
        }

        steps.push_back(archiving);
    }

    archives.insert(archives.end(), folder_archives.begin(), folder_archives.end());
    object_files = "-Wl,--whole-archive " + quoted_list(archives) + " -Wl,--no-whole-archive";
    return true;
}

bool plan_link(const property_table& properties, const std::vector<std::string>& objects,
               const std::string& core_archive, std::string_view warnings, std::vector<build_step>& folder_archives,
               build_step& step, std::string& error) {
    const std::string& build_path = properties.get("build.path");
    step = { step_kind::link, "recipe.c.combine.pattern", {}, "",
             join_path(build_path, properties.get("build.project_name") + ".elf"), {} };

    std::string object_files = quoted_list(objects);
    bool is_long = object_files.size() > LINKED_OBJECTS_MAX;
    if (is_long && !plan_folder_archives(properties, objects, folder_archives, object_files, error)) {
        return false;
    }

    property_table linking = properties;
    linking.set("compiler.warning_flags", properties.get("compiler.warning_flags." + std::string(warnings)));
    linking.set("archive_file", relative_path(build_path, core_archive));
    linking.set("archive_file_path", core_archive);
    linking.set("object_files", object_files);
    for (const std::string& key : recipe_keys(linking, "recipe.c.combine")) {
        std::vector<std::string> words;
        size_t length = 0;
        if (!recipe_words(linking, key, words, length, error)) {
            return false;
        }

        step.recipe = key;
        step.commands.push_back(words);
        step.lengths.push_back(length);
    }

    return true;
}

bool plan_objcopy(const property_table& properties, std::vector<build_step>& steps, std::string& error) {
    for (const std::string& key : recipe_keys(properties, "recipe.objcopy")) {
        std::vector<std::string> words;
        size_t length = 0;
        if (!recipe_words(properties, key, words, length, error)) {
            return false;
        }

        steps.push_back({ step_kind::objcopy, key, { words }, "", "", { length } });
    }

    return true;
}

static bool plan_tree(const property_table& properties, const std::string& folder, bool recursive,
                      const std::string& object_directory, const std::vector<std::string>& includes,
                      std::string_view warnings, std::vector<build_step>& compiles, std::string& error) {
    std::vector<std::string> sources;

    return find_source_files(folder, recursive, sources, error) &&
           plan_compiles(properties, sources, folder, object_directory, includes, warnings, compiles, error);
}

bool plan_build_compiles(const property_table& properties, const std::vector<std::string>& sketch_includes,
                         std::string_view warnings, compile_plan& plan, std::string& error) {
    const std::string& build_path = properties.get("build.path");
    std::string core_folder = properties.get("build.core.path");
    std::string variant_folder = properties.get("build.variant.path");
    std::string sketch_tree = join_path(build_path, SKETCH_FOLDER);
    std::string source_tree = join_path(sketch_tree, SKETCH_SOURCE_FOLDER);
    std::string core_tree = join_path(build_path, CORE_FOLDER);

    std::vector<std::string> core_includes = { core_folder };
    bool has_variant = !variant_folder.empty() && is_directory(variant_folder);
    if (has_variant) {
        core_includes.push_back(variant_folder);
    }

    return plan_tree(properties, sketch_tree, false, sketch_tree, sketch_includes, warnings, plan.sketch, error) &&
           (!is_directory(source_tree) ||
            plan_tree(properties, source_tree, true, source_tree, sketch_includes, warnings, plan.sources, error)) &&
           (!has_variant ||
            plan_tree(properties, variant_folder, true, core_tree, core_includes, NO_WARNINGS, plan.variant, error)) &&
           plan_tree(properties, core_folder, true, core_tree, core_includes, NO_WARNINGS, plan.core, error);
}

static void add_sorted_objects(const std::vector<build_step>& compiles, std::vector<std::string>& objects) {
    std::vector<std::string> group;
    for (const build_step& step : compiles) {
        group.push_back(step.output);
    }

    std::sort(group.begin(), group.end());
    objects.insert(objects.end(), group.begin(), group.end());
}

std::vector<std::string> linked_objects(const compile_plan& plan) {
    std::vector<std::string> objects;
    for (const std::vector<build_step>* group : { &plan.sketch, &plan.sources, &plan.variant }) {
        add_sorted_objects(*group, objects);
    }

    return objects;
}

size_t compiles_to_start(size_t waiting, uint32_t free_slots, bool failed) {
    return failed ? 0 : std::min<size_t>(waiting, free_slots);
}

bool runs_inside_mcufw(const std::vector<std::string>& words) {
    return words.size() >= 2 && words[0] == "mcufw" && words[1] == "hook";
}

size_t command_text_length(const build_step& step, size_t index) {
    if (index < step.lengths.size()) {
        return step.lengths[index];
    }

    const std::vector<std::string>& words = step.commands[index];
    size_t length = words.empty() ? 0 : words.size() - 1;
    for (const std::string& word : words) {
        length += word.size();
    }

    return length;
}

void shorten_long_command(const std::string& build_path, size_t text_length, std::vector<std::string>& words,
                          std::string& working_directory) {
    if (text_length <= COMMAND_LENGTH_MAX) {
        return;
    }

    working_directory = build_path;
    for (std::string& word : words) {
        struct stat status;
        if (stat(word.c_str(), &status) != 0 && errno == ENOENT) {
            continue;
        }

        if (word == build_path) {
            word = ".";
        } else if (word.starts_with(build_path + "/") && word.find("..") == std::string::npos) {
            word = word.substr(build_path.size() + 1);
        }
    }
}

bool plan_size(const property_table& properties, build_step& step, std::string& error) {
    std::vector<std::string> words;
    size_t length = 0;
    if (!recipe_words(properties, "recipe.size.pattern", words, length, error)) {
        return false;
    }

    step = { step_kind::size, "recipe.size.pattern", { words }, "", "", { length } };

    return true;
}
