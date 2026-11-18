#ifndef SHEEN_EXTENSIONS_C_RUN_COMPILER_DIAGNOSTICS_H
#define SHEEN_EXTENSIONS_C_RUN_COMPILER_DIAGNOSTICS_H

/* Pure C++20 compiler/linker diagnostic parser, C/C++ target discovery, and out-of-tree build plan helpers. */

#include "extensions/workspace_paths.h"

#include <string>
#include <string_view>
#include <vector>

namespace sheen {

struct compiler_environment {
    std::string workspace_root;
    std::string c_compiler;
    std::string cxx_compiler;
    bool has_src_dir = false;
    bool has_include_dir = false;
    bool uses_stellux_gcc = false;
    bool has_stellux_usr_include = false;
    bool has_stellux_libstlx = false;
};

struct compiler_step {
    std::string program;
    std::vector<std::string> arguments;

    bool operator==(const compiler_step&) const = default;
};

struct compiler_build_plan {
    std::string build_dir;
    std::string output_binary;
    std::vector<std::string> object_files;
    std::vector<compiler_step> steps;
};

/* Stateful line-streaming parser for GCC, Clang, TCC, and GNU ld diagnostic output. */
class compiler_output_parser {
public:
    explicit compiler_output_parser(std::string workspace_root, bool require_existing_files = false);

    void reset();
    bool feed(std::string_view chunk);
    bool finish();

    const std::vector<script_diagnostic>& diagnostics() const { return m_diagnostics; }
    int error_count() const;
    int warning_count() const;

private:
    bool process_line(std::string_view raw_line);
    void record_diagnostic(const script_diagnostic& item);

    std::string m_workspace_root;
    bool m_require_existing_files = false;
    std::string m_line_buffer;
    std::vector<script_diagnostic> m_diagnostics;
};

bool is_cpp_source_path(std::string_view path);
bool is_c_or_cpp_source_path(std::string_view path);

std::vector<std::string> discover_c_run_targets(const std::string& workspace_root);
std::vector<std::string> collect_project_c_cxx_sources(const std::string& workspace_root);

std::string compute_build_directory_path(std::string_view workspace_root);
std::string compute_target_output_binary_path(std::string_view workspace_root, std::string_view target_path);
std::string compute_project_output_binary_path(std::string_view workspace_root);
compiler_build_plan build_compiler_plan(const compiler_environment& env, const std::vector<std::string>& sources,
                                        bool is_project_build);

} // namespace sheen

#endif
