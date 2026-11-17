#ifndef MCUFW_BUILD_HPP
#define MCUFW_BUILD_HPP

// Builds a sketch in arduino-cli's order, each stage between its hook points, compiling up to `jobs` files at once.
// A recipe's or a `--else` command whose program is `mcufw hook` runs inside mcufw.

#include "boards.hpp"
#include "failure.hpp"
#include "layout.hpp"
#include "report.hpp"
#include "sketch.hpp"
#include "tools.hpp"

#include <cstdint>
#include <string>
#include <vector>

// The build tree's lock, held exclusively by whatever builds in the tree or deletes it
constexpr const char* BUILD_LOCK_NAME = "lock";

struct build_options {
    uint32_t    jobs;
    std::string warnings;  // none, default, more or all, for the sketch's files and the link
    bool        verbose;   // print each command and what it prints on stdout
};

struct build_result {
    std::string directory;
    std::string elf;
    std::string image;       // `recipe.output.tmp_file` in the build tree, else the ELF
    bool        up_to_date;  // no step that keeps a record ran, which `reporter::built` reports as up to date
};

// Builds `sketch` in its tree under `layout.builds`, holding the tree's lock for the call, so a second build of the
// sketch fails at once as `lock`. Reports the build through `report` and leaves `failed` for the caller to report.
bool build_sketch(
    const directory_layout& layout,
    const sketch& sketch,
    const selected_board& board,
    const std::vector<located_tool>& tools,
    const build_options& options,
    reporter& report,
    build_result& result,
    failure& failed
);

#endif // MCUFW_BUILD_HPP
