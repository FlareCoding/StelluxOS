#include "build.hpp"

#include "build_plan.hpp"
#include "cancel.hpp"
#include "compiler_output.hpp"
#include "file_lock.hpp"
#include "files.hpp"
#include "hooks.hpp"
#include "jobs.hpp"
#include "sizes.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

constexpr const char* LOCK_FILE = "lock";
constexpr const char* SKETCH_FOLDER = "sketch";
constexpr const char* CORE_FOLDER = "core";
constexpr const char* CORE_ARCHIVE = "core.a";

// What one build shares between its stages
struct build_run {
    const build_options&     options;
    reporter&                report;
    failure&                 failed;
    job_runner&              runner;
    property_table           properties;
    std::string              directory;
    std::vector<std::string> includes;          // the `{includes}` of the sketch's compiles, the core's and variant's
    uint64_t                 compiled = 0;      // compiles finished, for the progress count
    uint64_t                 compiles_total = 0;
    compile_plan             compiles;
};

static bool fail(build_run& run, failure_kind kind, const std::string& reason) {
    run.failed = { kind, reason };
    return false;
}

static bool fail_unless_cancelled(build_run& run, failure_kind kind, const std::string& reason) {
    if (cancel_requested()) {
        return fail(run, failure_kind::cancelled, "the build was cancelled");
    }

    return fail(run, kind, reason);
}

static const char* stage_of(step_kind kind) {
    switch (kind) {
    case step_kind::hook:
        return "hook";
    case step_kind::compile:
        return "compile";
    case step_kind::archive:
        return "archive";
    case step_kind::link:
        return "link";
    case step_kind::objcopy:
        return "objcopy";
    case step_kind::size:
        return "size";
    }

    return "hook";
}

static failure_kind failure_of(step_kind kind) {
    switch (kind) {
    case step_kind::hook:
        return failure_kind::recipe;
    case step_kind::compile:
        return failure_kind::compile;
    case step_kind::archive:
        return failure_kind::recipe;
    case step_kind::link:
        return failure_kind::link;
    case step_kind::objcopy:
        return failure_kind::recipe;
    case step_kind::size:
        return failure_kind::size;
    }

    return failure_kind::recipe;
}

static bool run_step(build_run& run, const build_step& step);

// A `mcufw hook` command, run inside mcufw. A copy-first finding no source runs its `--else` command as a hook step of
// its own, whose output is the copy-first's target.
static bool run_hook_words(build_run& run, const build_step& step, const std::vector<std::string>& words) {
    hook_command command;
    std::string error;
    bool run_otherwise = false;
    std::vector<std::string> operands(words.begin() + 2, words.end());
    if (!parse_hook(operands, command, error) || !run_hook(command, run_otherwise, error)) {
        return fail(run, failure_kind::recipe, step.recipe + ": " + error);
    }

    build_step otherwise = { step_kind::hook, step.recipe, { command.otherwise }, "", command.target, {} };

    return !run_otherwise || run_step(run, otherwise);
}

// Runs a step's command to its end and gives what it printed, a `mcufw hook` inside mcufw and any other through the
// runner. `--verbose` shows the command as it runs, after the long-command rule rewrote it.
static bool run_command(build_run& run, const build_step& step, size_t index, finished_job& finished) {
    finished = {};
    const std::vector<std::string>& words = step.commands[index];
    if (runs_inside_mcufw(words)) {
        if (run.options.verbose) {
            run.report.command_started(words);
        }

        return run_hook_words(run, step, words);
    }

    job started = { 0, words, "" };
    shorten_long_command(run.directory, command_text_length(step, index), started.words, started.working_directory);
    if (run.options.verbose) {
        run.report.command_started(started.words);
    }

    std::string error;
    if (!run.runner.start(started, error) || !run.runner.wait_for_next(finished, error)) {
        return fail_unless_cancelled(run, failure_of(step.kind), step.recipe + " cannot run: " + error);
    }

    return true;
}

// A command's stderr always shows and its stdout with `--verbose`, as arduino-cli shows them
static void show_tool_output(build_run& run, step_kind kind, const finished_job& finished) {
    if (run.options.verbose && !finished.output.empty()) {
        run.report.tool_output(stage_of(kind), finished.output);
    }

    if (!finished.error_output.empty()) {
        run.report.tool_output(stage_of(kind), finished.error_output);
    }
}

// Runs a step's commands in order
static bool run_step(build_run& run, const build_step& step) {
    for (size_t i = 0; i < step.commands.size(); i++) {
        finished_job finished;
        if (!run_command(run, step, i, finished)) {
            return false;
        }

        show_tool_output(run, step.kind, finished);
        std::string problem = exit_problem(finished.exit_status);
        if (!problem.empty()) {
            return fail(run, failure_of(step.kind), step.commands[i][0] + " " + problem + " in " + step.recipe);
        }

        if (cancel_requested()) {
            return fail(run, failure_kind::cancelled, "the build was cancelled");
        }
    }

    return true;
}

static bool run_steps(build_run& run, const std::vector<build_step>& steps) {
    for (const build_step& step : steps) {
        if (!run_step(run, step)) {
            return false;
        }
    }

    return true;
}

static bool run_hooks(build_run& run, std::string_view point) {
    std::vector<build_step> steps;
    std::string error;
    if (!plan_hooks(run.properties, point, steps, error)) {
        return fail(run, failure_kind::recipe, error);
    }

    return run_steps(run, steps);
}

// The failure's count of errors and the files they are in, such as `1 error in Lamp.ino`
static std::string describe_errors(const std::vector<compiler_problem>& errors) {
    std::vector<std::string> files;
    for (const compiler_problem& problem : errors) {
        std::string name = file_name(problem.file);
        if (std::find(files.begin(), files.end(), name) == files.end()) {
            files.push_back(name);
        }
    }

    std::string list;
    for (size_t i = 0; i < files.size(); i++) {
        list += (i == 0 ? "" : i + 1 == files.size() ? " and " : ", ") + files[i];
    }

    return std::to_string(errors.size()) + (errors.size() == 1 ? " error in " : " errors in ") + list;
}

// Shows a compiler's stderr and the problems it names, keeping the errors for the failure's sentence
static void show_compiler_output(build_run& run, const std::string& output, std::vector<compiler_problem>& errors) {
    if (!output.empty()) {
        run.report.tool_output("compile", output);
    }

    for (const compiler_problem& problem : find_compiler_problems(output)) {
        run.report.problem(problem);
        if (problem.severity == problem_severity::error) {
            errors.push_back(problem);
        }
    }
}

// Runs one group's compiles up to `jobs` at once, each one's output shown whole as it exits. After a failure no
// compile starts, the running ones finish, and the build fails naming the errors.
static bool run_compiles(build_run& run, const std::vector<build_step>& steps) {
    std::vector<compiler_problem> errors;
    std::string first_failure;
    size_t next = 0;
    while (next < steps.size() || !run.runner.idle()) {
        size_t starting = compiles_to_start(steps.size() - next, run.runner.free_slots(), !first_failure.empty());
        for (size_t started_now = 0; started_now < starting; started_now++) {
            const build_step& step = steps[next];
            job started = { next, step.commands[0], "" };
            shorten_long_command(run.directory, command_text_length(step, 0), started.words,
                                 started.working_directory);
            if (run.options.verbose) {
                run.report.command_started(started.words);
            }

            std::string error;
            if (!make_directories(parent_directory(step.output), error) || !run.runner.start(started, error)) {
                first_failure = "cannot compile " + file_name(step.source) + ": " + error;
                break;
            }

            next++;
        }

        if (run.runner.idle()) {
            break;
        }

        finished_job finished;
        std::string error;
        if (!run.runner.wait_for_next(finished, error)) {
            return fail_unless_cancelled(run, failure_kind::compile, error);
        }

        const build_step& step = steps[finished.id];
        run.compiled++;
        run.report.progress("compile", run.compiled, run.compiles_total, monotonic_clock_ns());
        if (run.options.verbose && !finished.output.empty()) {
            run.report.tool_output("compile", finished.output);
        }

        show_compiler_output(run, finished.error_output, errors);
        std::string problem = exit_problem(finished.exit_status);
        if (!problem.empty() && first_failure.empty()) {
            first_failure = step.commands[0][0] + " " + problem + " compiling " + file_name(step.source);
        }
    }

    if (first_failure.empty()) {
        return true;
    }

    return fail(run, failure_kind::compile, errors.empty() ? first_failure : describe_errors(errors));
}

// Writes the merged .ino text as `<main>.ino.cpp` and its `.merged` copy, and copies every other sketch file after a
// `#line 1` mark naming the original
static bool prepare_sketch_tree(build_run& run, const sketch& sketch) {
    std::string sketch_tree = join_path(run.directory, SKETCH_FOLDER);
    std::string error;
    merged_sketch merged;
    bool changed = false;
    std::string main_source = join_path(sketch_tree, sketch.main_file + ".cpp");
    if (!make_directories(sketch_tree, error) || !merge_ino_files(sketch, merged, error) ||
        !write_file_if_changed(main_source + ".merged", merged.text, changed, error) ||
        !write_file_if_changed(main_source, merged.text, changed, error)) {
        return fail(run, failure_kind::io, error);
    }

    std::vector<std::string> copied = sketch.source_files;
    copied.insert(copied.end(), sketch.copied_files.begin(), sketch.copied_files.end());
    for (const std::string& relative : copied) {
        std::string target = join_path(sketch_tree, relative);
        std::string text;
        if (!copied_file_text(sketch, relative, text, error) || !make_directories(parent_directory(target), error) ||
            !write_file_if_changed(target, text, changed, error)) {
            return fail(run, failure_kind::io, error);
        }
    }

    return true;
}

// Plans every compile before the first runs, so the compile total and the `Core` line print before any compile
static bool plan_and_count_compiles(build_run& run, const selected_board& board) {
    std::string error;
    compile_plan& plan = run.compiles;
    if (!plan_build_compiles(run.properties, run.includes, run.options.warnings, plan, error)) {
        return fail(run, failure_kind::compile, error);
    }

    std::string core_archive = join_path(join_path(run.directory, CORE_FOLDER), CORE_ARCHIVE);
    run.compiles_total = plan.sketch.size() + plan.sources.size() + plan.variant.size() + plan.core.size();
    run.report.core_compiling(*board.core, core_archive, plan.variant.size() + plan.core.size());
    run.report.progress("compile", 0, run.compiles_total, monotonic_clock_ns());

    return true;
}

// The size recipe's stdout summed by the core's regexes and checked against the board's limits as arduino-cli checks
// them. A recipe that cannot run or fails, and regexes that cannot sum its output, only warn.
static bool check_size(build_run& run) {
    const property_table& properties = run.properties;
    if (properties.get("upload.maximum_size").empty()) {
        return true;
    }

    uint64_t flash_max = 0;
    uint64_t ram_max = 0;
    std::string error;
    if (!read_size_limits(properties.get("upload.maximum_size"), properties.get("upload.maximum_data_size"), flash_max,
                          ram_max, error)) {
        return fail(run, failure_kind::size, error);
    }

    property_table sizing = properties;
    sizing.set("compiler.warning_flags", properties.get("compiler.warning_flags." + run.options.warnings));

    build_step step;
    finished_job finished;
    if (!plan_size(sizing, step, error) || !run_command(run, step, 0, finished)) {
        if (cancel_requested()) {
            return fail(run, failure_kind::cancelled, "the build was cancelled");
        }

        std::string reason = error.empty() ? run.failed.reason : error;
        run.report.tool_output("size", "Couldn't determine program size: " + reason);
        run.failed = {};
        return true;
    }

    if (!finished.error_output.empty()) {
        run.report.tool_output("size", finished.error_output);
    }

    program_size size;
    bool exited = exit_problem(finished.exit_status).empty();
    if (!exited || !sum_program_size(finished.output, properties.get("recipe.size.regex"),
                                     properties.get("recipe.size.regex.data"),
                                     properties.get("recipe.size.regex.eeprom"), size, error)) {
        run.report.tool_output("size", "Couldn't determine program size" + (error.empty() ? "" : ": " + error));
        return true;
    }

    size_verdict verdict = check_program_size(size, flash_max, ram_max, properties.get("build.warn_data_percentage"));
    run.report.size_checked(size, flash_max, ram_max);
    for (const std::string& warning : verdict.warnings) {
        run.report.tool_output("size", warning);
    }

    return verdict.failure.empty() || fail(run, failure_kind::size, verdict.failure);
}

static bool archive_core(build_run& run, const std::string& core_archive) {
    if (unlink(core_archive.c_str()) != 0 && errno != ENOENT) {
        return fail(run, failure_kind::io, "cannot remove " + core_archive + ": " + strerror(errno));
    }

    std::vector<std::string> objects;
    for (const build_step& step : run.compiles.core) {
        objects.push_back(step.output);
    }

    std::sort(objects.begin(), objects.end());
    build_step archive;
    std::string error;
    if (!plan_archive(run.properties, objects, core_archive, archive, error)) {
        return fail(run, failure_kind::recipe, error);
    }

    return run_step(run, archive);
}

// A long link's folder archives, each emptied first, since `ar` adds to an archive and would keep an older build's
// members. A failing one only shows its output, as arduino-cli's linker goes on to the link.
static bool run_folder_archives(build_run& run, const std::vector<build_step>& archives) {
    for (const build_step& archive : archives) {
        if (unlink(archive.output.c_str()) != 0 && errno != ENOENT) {
            return fail(run, failure_kind::io, "cannot remove " + archive.output + ": " + strerror(errno));
        }

        for (size_t i = 0; i < archive.commands.size(); i++) {
            finished_job finished;
            if (!run_command(run, archive, i, finished)) {
                return false;
            }

            show_tool_output(run, archive.kind, finished);
            if (cancel_requested()) {
                return fail(run, failure_kind::cancelled, "the build was cancelled");
            }
        }
    }

    return true;
}

// The link of the sketch's objects, then the variant's, with the core
static bool link_sketch(build_run& run, const std::string& core_archive) {
    std::vector<build_step> folder_archives;
    build_step link;
    std::string error;
    if (!plan_link(run.properties, linked_objects(run.compiles), core_archive, run.options.warnings, folder_archives,
                   link, error)) {
        return fail(run, failure_kind::link, error);
    }

    return run_folder_archives(run, folder_archives) && run_step(run, link);
}

static bool make_images(build_run& run) {
    std::vector<build_step> objcopy;
    std::string error;
    if (!plan_objcopy(run.properties, objcopy, error)) {
        return fail(run, failure_kind::recipe, error);
    }

    return run_steps(run, objcopy);
}

static bool run_stage(build_run& run, const stage_step& stage, const sketch& sketch, const selected_board& board) {
    std::string core_archive = join_path(join_path(run.directory, CORE_FOLDER), CORE_ARCHIVE);
    switch (stage.stage) {
    case build_stage::hooks:
        return run_hooks(run, stage.hook_point);
    case build_stage::sketch_tree:
        return prepare_sketch_tree(run, sketch);
    case build_stage::sketch_compiles:
        return plan_and_count_compiles(run, board) && run_compiles(run, run.compiles.sketch) &&
               run_compiles(run, run.compiles.sources);
    case build_stage::core_compiles:
        return run_compiles(run, run.compiles.variant) && run_compiles(run, run.compiles.core);
    case build_stage::core_archive:
        return archive_core(run, core_archive);
    case build_stage::link:
        return link_sketch(run, core_archive);
    case build_stage::objcopy:
        return make_images(run);
    case build_stage::size:
        return check_size(run);
    }

    return true;
}

static bool build_in_tree(build_run& run, const sketch& sketch, const selected_board& board) {
    if (run.properties.find("recipe.advanced_size.pattern")) {
        return fail(run, failure_kind::core, "the core sets recipe.advanced_size.pattern, which mcufw does not run");
    }

    std::string variant_folder = run.properties.get("build.variant.path");
    run.includes = { run.properties.get("build.core.path") };
    if (!variant_folder.empty()) {
        run.includes.push_back(variant_folder);
    }

    for (const stage_step& stage : build_stages()) {
        if (!run_stage(run, stage, sketch, board)) {
            return false;
        }
    }

    return true;
}

bool build_sketch(
    const directory_layout& layout,
    const sketch& sketch,
    const selected_board& board,
    const std::vector<located_tool>& tools,
    const build_options& options,
    reporter& report,
    build_result& result,
    failure& failed
) {
    std::string directory = build_directory_for(layout, sketch.folder);
    std::string error;
    if (!make_directories(directory, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    file_lock lock;
    if (!lock.acquire(join_path(directory, LOCK_FILE), lock_mode::exclusive, error)) {
        failed = { failure_kind::lock, error };
        return false;
    }

    job_runner runner(options.jobs);
    property_table properties = build_properties(board_properties(board, tools), sketch, directory);
    build_run run = { options, report, failed, runner, std::move(properties), directory, {}, 0, 0, {} };
    if (!build_in_tree(run, sketch, board)) {
        return false;
    }

    std::string project = run.properties.get("build.project_name");
    std::string image = run.properties.expand(run.properties.get("recipe.output.tmp_file"));
    result.directory = directory;
    result.elf = join_path(directory, project + ".elf");
    result.image = image.empty() ? result.elf : join_path(directory, image);

    return true;
}
