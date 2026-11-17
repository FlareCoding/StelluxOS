#ifndef MCUFW_BUILD_PLAN_HPP
#define MCUFW_BUILD_PLAN_HPP

// The commands a build runs and their order, worked out from the board's properties as arduino-cli's builder works them
// out. The build tree is laid out as arduino-cli lays it out, since the core's recipes name its paths.

#include "boards.hpp"
#include "layout.hpp"
#include "sketch.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class step_kind { hook, compile, archive, link, objcopy, size };

// A build's stages, each run as a step of its own or as the recipes of a hook point
enum class build_stage { hooks, sketch_tree, sketch_compiles, core_compiles, core_archive, link, objcopy, size };

struct stage_step {
    build_stage stage;
    const char* hook_point;  // the point a `hooks` stage runs, such as `core.prebuild`, else empty
};

// One unit of work, whose commands run in order: one per object for an archive, and one for any other step
struct build_step {
    step_kind                             kind;
    std::string                           recipe;    // the recipe key it expands, which failures name
    std::vector<std::vector<std::string>> commands;  // each the program first
    std::string                           source;    // a compile's source file, else empty
    std::string                           output;    // the object, archive, ELF or copy-first target made, else empty
    std::vector<size_t>                   lengths;   // each command's expanded recipe text, empty for none
};

// A build's compiles, in the groups arduino-cli compiles one after another
struct compile_plan {
    std::vector<build_step> sketch;   // the sketch tree's own files
    std::vector<build_step> sources;  // its src/ folder's
    std::vector<build_step> variant;
    std::vector<build_step> core;
};

// The build's stages in arduino-cli's order
std::vector<stage_step> build_stages();

// `<builds>/<folder's name>-<the first 8 hex digits of the SHA-256 of the folder's absolute path>`, computed from the
// path alone, so a folder that no longer exists still has one
std::string build_directory_for(const directory_layout& layout, const std::string& sketch_folder);

// The board's properties, then the build tree's paths and the values Arduino's builder adds for a build
property_table build_properties(const property_table& board_properties, const sketch& sketch,
                                const std::string& build_directory);

// Each hook recipe at `point`, such as `prebuild` or `core.postbuild`, as a step
bool plan_hooks(const property_table& properties, std::string_view point, std::vector<build_step>& steps,
                std::string& error);

// One compile step per source, its object and .d under `object_directory` at the source's path relative to
// `source_root`, with the warning flags of `warnings` and `include_directories` as `{includes}`
bool plan_compiles(const property_table& properties, const std::vector<std::string>& sources,
                   const std::string& source_root, const std::string& object_directory,
                   const std::vector<std::string>& include_directories, std::string_view warnings,
                   std::vector<build_step>& steps, std::string& error);

// Every compile of the build: the sketch tree's and its src/ folder's at `warnings`, then the variant's and the core's
// into `core/` at `none`, as approved design decision 6 has it where arduino-cli uses one level for every compile
bool plan_build_compiles(const property_table& properties, const std::vector<std::string>& sketch_includes,
                         std::string_view warnings, compile_plan& plan, std::string& error);

// The objects the link takes, in arduino-cli's order: the sketch's, its src/ folder's, then the variant's, each group
// sorted. The core's go into `core.a`.
std::vector<std::string> linked_objects(const compile_plan& plan);

// How many of the `waiting` compiles may start: none after a failure, so the running ones finish alone
size_t compiles_to_start(size_t waiting, uint32_t free_slots, bool failed);

// The archive step adding each object to `archive`, one command per object, as arduino-cli's builder runs them
bool plan_archive(const property_table& properties, const std::vector<std::string>& objects,
                  const std::string& archive, build_step& step, std::string& error);

// The link of `objects` with `core_archive`. Past 30,000 characters of quoted objects, each folder's objects first go
// into its own `objs.a`, planned as `folder_archives`, which the link takes whole, as arduino-cli's linker does.
bool plan_link(const property_table& properties, const std::vector<std::string>& objects,
               const std::string& core_archive, std::string_view warnings, std::vector<build_step>& folder_archives,
               build_step& step, std::string& error);

// The objcopy recipes, which make the images a board is flashed with
bool plan_objcopy(const property_table& properties, std::vector<build_step>& steps, std::string& error);

// Whether `words` is a `mcufw hook` command, which the build runs inside mcufw
bool runs_inside_mcufw(const std::vector<std::string>& words);

// The text arduino-cli's 30,000-character rule measures for a step's command: its expanded recipe, or its words joined
// by spaces for a command no recipe expands, such as a copy-first's `--else`
size_t command_text_length(const build_step& step, size_t index);

// arduino-cli's rule for a command whose text passes 30,000 characters: each word naming an existing file inside the
// build tree is made relative to it, and the command runs there
void shorten_long_command(const std::string& build_path, size_t text_length, std::vector<std::string>& words,
                          std::string& working_directory);

// The size recipe's command, whose stdout the core's regexes sum
bool plan_size(const property_table& properties, build_step& step, std::string& error);

#endif // MCUFW_BUILD_PLAN_HPP
