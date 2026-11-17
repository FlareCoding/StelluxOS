#include "build.hpp"

#include "build_plan.hpp"
#include "cancel.hpp"
#include "compiler_output.hpp"
#include "core_cache.hpp"
#include "discovery.hpp"
#include "discovery_replay.hpp"
#include "file_lock.hpp"
#include "files.hpp"
#include "hooks.hpp"
#include "indexes.hpp"
#include "jobs.hpp"
#include "library_hint.hpp"
#include "prototypes.hpp"
#include "recipes.hpp"
#include "sizes.hpp"
#include "step_records.hpp"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>

constexpr const char* SKETCH_FOLDER = "sketch";
constexpr const char* CORE_FOLDER = "core";
constexpr const char* CORE_LIBRARIES_FOLDER = "libraries";
constexpr const char* PREPROCESSED_FOLDER = "preproc";
constexpr const char* PREPROCESSED_FILE = "sketch_merged.cpp";
constexpr int64_t     NS_PER_SECOND = 1'000'000'000;

enum compile_stage { SKETCH_STAGE, LIBRARIES_STAGE, CORE_STAGE, STAGE_COUNT };

// A stage's compiles that must run, with no record or one that no longer matches. Each stage decides after the hooks
// before it, which may rewrite a response file its compiles read, and the count before then is an estimate.
struct needed_compiles {
    uint64_t    count = 0;
    std::string reason;  // the reason of the first of them that has one
};

// Discovery's probes running ahead of the turn by their words, and those that finished before their turn. Probing
// ahead starts once a file completes alone.
struct probing {
    std::map<std::vector<std::string>, uint64_t> ahead;
    std::map<uint64_t, finished_job>             finished;
    uint64_t                                     last_id = 0;
    bool                                         ahead_started = false;
};

// The core's place in the core cache. `entry` is empty for a core with nothing to compile or one kept to its sketch.
struct cached_core {
    std::string entry;
    bool        hit = false;  // a build placed the entry's archive already
};

// What one build shares between its stages
struct build_run {
    const directory_layout&     layout;
    const build_options&        options;
    reporter&                   report;
    failure&                    failed;
    job_runner&                 runner;
    property_table              properties;
    std::string                 directory;
    std::vector<std::string>    includes;          // the `{includes}` of the sketch's compiles
    std::vector<library>        libraries;         // every library discovery may add
    std::vector<const library*> added;             // those it added, in the order they joined
    uint64_t                    compiled = 0;      // compiles finished, for the progress count
    uint64_t                    compiles_total = 0;
    compile_plan                compiles;
    merged_sketch               merged;
    needed_compiles             needed[STAGE_COUNT];
    bool                        ran_step = false;  // a step that keeps a record ran, so the build is not up to date
    std::string                 main_file;         // the sketch's main .ino file, the merged C++'s name for a person
    bool                        merges_ino_files;  // the sketch has other .ino files merged with it
    bool                        sketch_replayed;   // discovery's record answered every probe of the sketch's files
    std::vector<build_step>     sketch_needed;     // the sketch tree's compiles that must run
    std::vector<build_step>     sources_needed;    // its src/ folder's
    cached_core                 cached;
    std::string                 core_archive;      // the archive the link takes: the tree's, or the cache's
};

// The clock that file modification times use, since a record compares a step's start with them
static int64_t wall_clock_ns() {
    timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<int64_t>(now.tv_sec) * NS_PER_SECOND + now.tv_nsec;
}

static bool fail(build_run& run, failure_kind kind, const std::string& reason) {
    run.failed = { kind, reason };
    return false;
}

// Whether `step` must run: it keeps no record, or its record no longer matches for `reason`
static bool step_needed(build_run& run, const build_step& step, bool& needed, std::string& reason) {
    std::string error;
    needed = !step_is_current(step, run.directory, reason, error);
    return error.empty() || fail(run, failure_kind::io, error);
}

static bool record_step(build_run& run, const build_step& step, int64_t started_ns) {
    std::string error;
    run.ran_step = run.ran_step || !step_record_path(step, run.directory).empty();
    return record_finished_step(step, run.directory, started_ns, error) || fail(run, failure_kind::io, error);
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
// its own, whose output is the copy-first's target unless the `--else` command is itself `mcufw hook`.
static bool run_hook_words(build_run& run, const build_step& step, const std::vector<std::string>& words) {
    hook_command command;
    std::string error;
    bool run_otherwise = false;
    std::vector<std::string> operands(words.begin() + 2, words.end());
    if (!parse_hook(operands, command, error) || !run_hook(command, run_otherwise, error)) {
        return fail(run, failure_kind::recipe, step.recipe + ": " + error);
    }

    // A `--else` that is itself `mcufw hook` keeps no record, since a file it copies may appear between builds
    std::string target = runs_inside_mcufw(command.otherwise) ? "" : command.target;
    build_step otherwise = { step_kind::hook, step.recipe, { command.otherwise }, "", target, {} };

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

// Runs a step's commands in order and records it once they succeed
static bool run_step_commands(build_run& run, const build_step& step) {
    int64_t started_ns = wall_clock_ns();
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

    return record_step(run, step, started_ns);
}

// Runs a step unless its record still matches
static bool run_step(build_run& run, const build_step& step) {
    bool needed = true;
    std::string reason;
    return step_needed(run, step, needed, reason) && (!needed || run_step_commands(run, step));
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

static uint64_t compiles_needed(const build_run& run) {
    uint64_t needed = 0;
    for (const needed_compiles& stage : run.needed) {
        needed += stage.count;
    }

    return needed;
}

// Runs one group's compiles up to `jobs` at once, each shown whole and recorded once it succeeds. After a failure none
// starts and the build fails naming the errors once the running ones finish. `compile_core` reports the last count.
static bool run_compiles(build_run& run, const std::vector<build_step>& steps) {
    std::vector<compiler_problem> errors;
    std::vector<int64_t> started_ns(steps.size());
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
            started_ns[next] = wall_clock_ns();
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
        if (run.compiled < compiles_needed(run)) {
            run.report.progress("compile", run.compiled, compiles_needed(run), monotonic_clock_ns());
        }

        if (run.options.verbose && !finished.output.empty()) {
            run.report.tool_output("compile", finished.output);
        }

        show_compiler_output(run, finished.error_output, errors);
        std::string problem = exit_problem(finished.exit_status);
        if (!problem.empty() && first_failure.empty()) {
            first_failure = step.commands[0][0] + " " + problem + " compiling " + file_name(step.source);
        }

        if (problem.empty() && !record_step(run, step, started_ns[finished.id])) {
            return false;
        }
    }

    if (first_failure.empty()) {
        return true;
    }

    return fail(run, failure_kind::compile, errors.empty() ? first_failure : describe_errors(errors));
}

static std::vector<std::string> objects_of(const std::vector<build_step>& compiles) {
    std::vector<std::string> objects;
    for (const build_step& step : compiles) {
        objects.push_back(step.output);
    }

    return objects;
}

// A compile's reason in the names a person knows: the merged sketch's C++ is named as the sketch's .ino file, or as
// its .ino files when there are several
static std::string reason_for_person(const build_run& run, const build_step& compile, const std::string& reason) {
    std::string source = file_name(compile.source);
    bool is_merged_sketch = source == run.main_file + ".cpp";
    if (is_merged_sketch && reason == COMMAND_CHANGED_REASON) {
        return run.main_file + "'s command changed";
    }

    if (is_merged_sketch && reason == source + " changed") {
        return run.merges_ino_files ? "the sketch's .ino files changed" : run.main_file + " changed";
    }

    return reason.starts_with("its ") ? source + "'s " + reason.substr(4) : reason;
}

// The compiles of `steps` that must run, counted into `decided`
static bool select_needed(build_run& run, const std::vector<build_step>& steps, needed_compiles& decided,
                          std::vector<build_step>& needed) {
    for (const build_step& step : steps) {
        bool is_needed = true;
        std::string reason;
        if (!step_needed(run, step, is_needed, reason)) {
            return false;
        }

        if (!is_needed) {
            continue;
        }

        if (decided.reason.empty() && !reason.empty()) {
            decided.reason = reason_for_person(run, step, reason);
        }

        decided.count++;
        needed.push_back(step);
    }

    return true;
}

static void report_compiles_needed(build_run& run) {
    std::string reason;
    for (const needed_compiles& stage : run.needed) {
        reason = reason.empty() ? stage.reason : reason;
    }

    run.report.compiles_needed(compiles_needed(run), run.compiles_total, reason);
}

// Replaces a stage's estimate with what it decided, reporting the count again when they differ
static void settle_stage(build_run& run, compile_stage stage, const needed_compiles& decided) {
    bool changed = decided.count != run.needed[stage].count || decided.reason != run.needed[stage].reason;
    run.needed[stage] = decided;
    if (changed) {
        report_compiles_needed(run);
    }
}

// Writes the merged .ino text as `<main>.ino.cpp.merged`, and copies every other sketch file after a `#line 1` mark
// naming the original
static bool prepare_sketch_tree(build_run& run, const sketch& sketch) {
    std::string sketch_tree = join_path(run.directory, SKETCH_FOLDER);
    std::string error;
    bool changed = false;
    std::string merged_file = join_path(sketch_tree, sketch.main_file + ".cpp.merged");
    if (!make_directories(sketch_tree, error) || !merge_ino_files(sketch, run.merged, error) ||
        !write_file_if_changed(merged_file, run.merged.text, changed, error)) {
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

// Starts a probe as arduino-cli's preprocessor runs, outside the long-command rule
static bool start_probe(build_run& run, const probe& started, uint64_t id) {
    if (!started.error.empty()) {
        return fail(run, failure_kind::compile, started.error);
    }

    if (run.options.verbose) {
        run.report.command_started(started.words);
    }

    job probing = { id, started.words, "" };
    std::string error;
    if (!make_directories(parent_directory(started.depfile), error) || !run.runner.start(probing, error)) {
        return fail(run, failure_kind::compile, "cannot probe " + file_name(started.source) + ": " + error);
    }

    return true;
}

// Waits for job `id`, keeping the results of jobs that finish first for their own turns
static bool wait_for_job(build_run& run, uint64_t id, std::map<uint64_t, finished_job>& finished,
                         finished_job& result) {
    while (finished.find(id) == finished.end()) {
        finished_job next;
        std::string error;
        if (!run.runner.wait_for_next(next, error)) {
            return fail_unless_cancelled(run, failure_kind::compile, error);
        }

        finished[next.id] = std::move(next);
    }

    result = std::move(finished[id]);
    finished.erase(id);

    return true;
}

static bool fail_unresolved(build_run& run, const selected_board& board, const std::string& header) {
    library_suggestion suggestion;
    std::string error;
    std::string index = join_path(run.layout.state, LIBRARY_INDEX_FILE);

    // A missing or unreadable index only leaves the hint out
    if (suggest_library(index, header, board.core->architecture, suggestion, error)) {
        run.report.library_suggested(header, suggestion);
    }

    return fail(run, failure_kind::library, "no installed library provides " + header);
}

static bool fail_discovery(build_run& run, const selected_board& board, const discovery_failure& failure) {
    if (failure.stop == discovery_stop::unreadable) {
        return fail(run, failure_kind::io, failure.output);
    }

    std::vector<compiler_problem> errors;
    show_compiler_output(run, failure.output, errors);
    switch (failure.stop) {
    case discovery_stop::unresolved:
        return fail_unresolved(run, board, failure.header);
    case discovery_stop::already_added:
        return fail(run, failure_kind::library, failure.header + " is missing though " + failure.library +
                                                    " provides it");
    case discovery_stop::refused:
        return fail(run, failure_kind::library, failure.library + ", and the sketch needs it for " + failure.header);
    case discovery_stop::unreadable:
    case discovery_stop::failed:
        break;
    }

    return fail(run, failure_kind::compile, "the board's preprocessor failed without saying why");
}

// arduino-cli's warning for a library made for neither the board's architecture nor one the board asks to accept
static void warn_about_architectures(build_run& run, const selected_board& board) {
    std::vector<std::string> architectures =
        board_architectures(board.core->architecture, run.properties.get("architecture.override_check"));
    for (const library* incompatible : incompatible_libraries(run.added, architectures)) {
        run.report.library_incompatible(*incompatible, architectures);
    }
}

// The turn's probe as arduino-cli's runner runs it: a probe ahead with its words counts, else every probe ahead stops
// and it runs alone
static bool probe_turn(build_run& run, const probe& turn, probing& state, probe_result& probed, bool& ran_alone) {
    auto running = state.ahead.find(turn.words);
    ran_alone = running == state.ahead.end();
    if (ran_alone) {
        run.runner.stop_all();
        state.ahead.clear();
        state.finished.clear();
    }

    finished_job result;
    uint64_t id = ran_alone ? ++state.last_id : running->second;
    if ((ran_alone && !start_probe(run, turn, id)) || !wait_for_job(run, id, state.finished, result)) {
        return false;
    }

    probed = read_probe_result(result.exit_status, result.error_output);
    return true;
}

// The turns of arduino-cli's library discovery, the record answering each until the first it cannot. Tells whether
// it answered every probe of the sketch's own files, which come first.
static bool take_turns(build_run& run, const selected_board& board, const std::vector<std::string>& sketch_sources,
                       library_discovery& discovery, discovery_replay& replay, bool& sketch_replayed) {
    probing state;
    sketch_replayed = true;
    while (const probe* turn = discovery.turn_probe()) {
        run.report.progress("libraries", discovery.files_done(), discovery.files_queued(), monotonic_clock_ns());
        probe_result probed;
        bool replayed = replay.answer(*turn, probed);
        bool is_sketch_file =
            std::find(sketch_sources.begin(), sketch_sources.end(), turn->source) != sketch_sources.end();
        sketch_replayed = sketch_replayed && (replayed || !is_sketch_file);

        bool ran_alone = false;
        if (!replayed && !probe_turn(run, *turn, state, probed, ran_alone)) {
            return false;
        }

        discovery_failure failure;
        probe_result again;
        if (!discovery.take_turn_result(probed, failure)) {
            run.runner.stop_all();

            // A recorded missing header carries no messages, so a failed turn the record answered is probed again for
            // them, as arduino-cli does for a header no library resolves
            if (replayed && !probe_turn(run, *turn, state, again, ran_alone)) {
                return false;
            }

            failure.output = replayed ? again.output : failure.output;
            return fail_discovery(run, board, failure);
        }

        bool completed = probed.status == probe_status::complete;
        state.ahead_started = state.ahead_started || (ran_alone && completed);
        if (!state.ahead_started || !completed) {
            continue;
        }

        for (const probe& later : discovery.probes_ahead(run.runner.free_slots())) {
            if (!start_probe(run, later, ++state.last_id)) {
                return false;
            }

            state.ahead[later.words] = state.last_id;
        }
    }

    run.runner.stop_all();
    return true;
}

// arduino-cli's library discovery, its record `<build>/libraries.record` kept only when it finishes, as arduino-cli
// removes its includes cache on a failure
static bool discover_libraries(build_run& run, const sketch& sketch, const selected_board& board) {
    std::vector<std::string> sources;
    std::string error;
    if (!discovery_sources(join_path(run.directory, SKETCH_FOLDER), sketch.main_file, sources, error)) {
        return fail(run, failure_kind::io, error);
    }

    if (!find_libraries(join_path(board.core->directory, CORE_LIBRARIES_FOLDER), run.layout, run.libraries, error)) {
        return fail(run, failure_kind::library, error);
    }

    std::string record = join_path(run.directory, DISCOVERY_RECORD_NAME);
    discovery_replay replay;
    if (!replay.load(record, error)) {
        return fail(run, failure_kind::io, error);
    }

    library_discovery discovery(run.properties, run.libraries, board.core->architecture, run.directory, sources,
                                run.includes);
    if (!take_turns(run, board, sources, discovery, replay, run.sketch_replayed)) {
        unlink(record.c_str());
        return false;
    }

    if (!save_discovery_record(record, discovery.reached(), error)) {
        return fail(run, failure_kind::io, error);
    }

    run.report.progress("libraries", discovery.files_done(), discovery.files_queued(), monotonic_clock_ns());
    run.includes = discovery.include_directories();
    run.added = discovery.libraries_added();

    run.report.libraries_found(run.added);
    warn_about_architectures(run, board);

    return true;
}

// Whether the file at `path` changed no earlier than the one at `than`, both existing
static bool is_no_older(const std::string& path, const std::string& than) {
    uint64_t size = 0;
    int64_t path_ns = 0;
    int64_t than_ns = 0;
    return read_file_size_and_time(path, size, path_ns) && read_file_size_and_time(than, size, than_ns) &&
           path_ns >= than_ns;
}

// Writes `<main>.ino.cpp`, the merged sketch with arduino-cli's prototypes, only when its content changes. The
// preprocessor runs as arduino-cli builds it, outside the long-command rule.
static bool add_prototypes(build_run& run, const sketch& sketch) {
    std::string sketch_tree = join_path(run.directory, SKETCH_FOLDER);
    std::string merged_file = join_path(sketch_tree, sketch.main_file + ".cpp.merged");
    std::string source_file = join_path(sketch_tree, sketch.main_file + ".cpp");

    // Skipped while the C++ is no older than the merge and the record answered each sketch file's probes, where
    // arduino-cli checks the merged sketch's .libsdetect.d
    if (run.sketch_replayed && is_no_older(source_file, merged_file)) {
        return true;
    }

    std::string preprocessed_file = join_path(join_path(run.directory, PREPROCESSED_FOLDER), PREPROCESSED_FILE);
    build_step step = { step_kind::compile, "recipe.preproc.macros", {}, merged_file, preprocessed_file, { 0 } };

    std::vector<std::string> words;
    std::string error;
    if (!make_directories(parent_directory(preprocessed_file), error) ||
        !preprocess_words(run.properties, merged_file, preprocessed_file, run.includes, "", words, error)) {
        return fail(run, failure_kind::compile, error);
    }

    finished_job finished;
    step.commands.push_back(words);
    bool ran = run_command(run, step, 0, finished);

    std::string preprocessed;
    bool read = ran && read_file(preprocessed_file, preprocessed, error);
    unlink(preprocessed_file.c_str());
    if (!ran) {
        return false;
    }

    std::vector<compiler_problem> errors;
    show_compiler_output(run, finished.error_output, errors);
    std::string problem = exit_problem(finished.exit_status);
    if (!problem.empty()) {
        std::string reason = words[0] + " " + problem + " preprocessing " + sketch.main_file;
        return fail(run, failure_kind::compile, errors.empty() ? reason : describe_errors(errors));
    }

    if (!read) {
        return fail(run, failure_kind::io, error);
    }

    std::vector<std::string> ino_files = { join_path(sketch.folder, sketch.main_file) };
    std::vector<ino_text> files;
    for (const std::string& name : sketch.other_ino_files) {
        ino_files.push_back(join_path(sketch.folder, name));
    }

    for (const std::string& path : ino_files) {
        std::string text;
        std::string ignored;
        read_file(path, text, ignored);
        files.push_back({ path, text });
    }

    bool line_too_long = false;
    std::string lines = sketch_lines(preprocessed, ino_files, line_too_long);

    // arduino-cli 1.6.0 also only reports this and goes on with the lines read before the long one
    if (line_too_long) {
        run.report.tool_output("compile", "An error occurred adding prototypes: line too long");
    }

    std::string source;
    bool changed = false;
    prototype_plan plan = find_prototypes(lines, ino_files[0], files);

    // A first function past the merged text leaves `<main>.ino.cpp` unwritten, as arduino-cli 1.6.0 leaves it
    if (insert_prototypes(run.merged, plan, source) && !write_file_if_changed(source_file, source, changed, error)) {
        return fail(run, failure_kind::io, error);
    }

    return true;
}

// The variant's compiles, and the core's unless the core cache holds its archive
static std::vector<build_step> core_stage_compiles(const compile_plan& plan, const cached_core& cached) {
    std::vector<build_step> steps = plan.variant;
    if (!cached.hit) {
        steps.insert(steps.end(), plan.core.begin(), plan.core.end());
    }

    return steps;
}

// The core's cache entry, keyed after the `prebuild` and `sketch.prebuild` hooks and before `core.prebuild`, whose
// words the key covers, with the compiler's version line read as one job
static bool look_up_core(build_run& run, const sketch& sketch, const selected_board& board) {
    const compile_plan& plan = run.compiles;
    if (plan.core.empty()) {
        return true;
    }

    std::vector<std::string> objects = objects_of(plan.core);
    std::sort(objects.begin(), objects.end());
    std::vector<build_step> key_steps = plan.core;
    build_step archiving;
    std::string error;
    if (!plan_archive(run.properties, objects, run.core_archive, archiving, error) ||
        !plan_hooks(run.properties, "core.prebuild", key_steps, error) ||
        !plan_hooks(run.properties, "core.postbuild", key_steps, error)) {
        return fail(run, failure_kind::recipe, error);
    }

    key_steps.push_back(archiving);

    const std::string& compiler = plan.core.front().commands[0][0];
    job version = { 0, { compiler, "--version" }, "" };
    finished_job finished;
    bool ran = run.runner.start(version, error) && run.runner.wait_for_next(finished, error);
    std::string problem = ran ? exit_problem(finished.exit_status) : error;
    if (!problem.empty()) {
        std::string reason = "cannot read the version of " + compiler + ": " + problem;
        return fail_unless_cancelled(run, failure_kind::compile, reason);
    }

    std::string key;
    std::string version_line = finished.output.substr(0, finished.output.find('\n'));
    if (!core_cache_key(key_steps, run.directory, sketch.folder, version_line, key, error)) {
        return fail(run, failure_kind::io, error);
    }

    cached_core& cached = run.cached;
    cached.entry = core_cache_entry(run.layout, board.definition->id, key);
    cached.hit = is_file(join_path(cached.entry, CORE_ARCHIVE_NAME));
    if (!cached.hit) {
        return true;
    }

    // A sketch that shadows a header the cached core read, or whose core in this tree read its folder, keeps a core of
    // its own
    std::vector<std::string> header_names;
    bool shadows = true;
    bool reads = false;
    if ((published_header_names(cached.entry, header_names) &&
         !sketch_shadows(sketch.folder, header_names, shadows, error)) ||
        !core_reads_sketch(plan.core, sketch.folder, false, reads, error)) {
        return fail(run, failure_kind::io, error);
    }

    if (shadows || reads) {
        cached = {};
    }

    return true;
}

static std::vector<build_step> library_steps(const library_compiles& compiles) {
    std::vector<build_step> steps = compiles.sources;
    steps.insert(steps.end(), compiles.utility.begin(), compiles.utility.end());
    return steps;
}

// The count before any compile runs: the sketch's compiles decided, the libraries' as their records stand, and the
// variant's and, without a cached core, the core's with no record, since those match only after `core.prebuild` runs
static bool estimate_compiles(build_run& run) {
    const compile_plan& plan = run.compiles;
    if (!select_needed(run, plan.sketch, run.needed[SKETCH_STAGE], run.sketch_needed) ||
        !select_needed(run, plan.sources, run.needed[SKETCH_STAGE], run.sources_needed)) {
        return false;
    }

    std::vector<build_step> estimated;
    for (const library_compiles& compiles : plan.libraries) {
        if (!select_needed(run, library_steps(compiles), run.needed[LIBRARIES_STAGE], estimated)) {
            return false;
        }
    }

    for (const build_step& step : core_stage_compiles(plan, run.cached)) {
        run.needed[CORE_STAGE].count += is_file(step_record_path(step, run.directory)) ? 0 : 1;
    }

    report_compiles_needed(run);
    return true;
}

// Plans every compile and looks the core up in the cache before the first compile runs, so the compile count and the
// `Core` line print before any compile
static bool plan_and_count_compiles(build_run& run, const sketch& sketch, const selected_board& board) {
    std::string error;
    compile_plan& plan = run.compiles;
    if (!plan_build_compiles(run.properties, run.includes, run.added, run.options.warnings, plan, error)) {
        return fail(run, failure_kind::compile, error);
    }

    if (!look_up_core(run, sketch, board)) {
        return false;
    }

    size_t core_compiles = core_stage_compiles(plan, run.cached).size();
    run.compiles_total = plan.sketch.size() + plan.sources.size() + core_compiles;
    for (const library_compiles& compiles : plan.libraries) {
        run.compiles_total += compiles.sources.size() + compiles.utility.size();
    }

    if (!estimate_compiles(run)) {
        return false;
    }

    if (run.cached.hit) {
        run.report.core_cached(join_path(run.cached.entry, CORE_ARCHIVE_NAME));
    } else {
        run.report.core_compiling(*board.core, run.core_archive, core_compiles);
    }

    if (compiles_needed(run) > 0) {
        run.report.progress("compile", 0, compiles_needed(run), monotonic_clock_ns());
    }

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

// An archive that must run is made from empty, since `ar` only adds and replaces
static bool archive_objects(build_run& run, std::vector<std::string> objects, const std::string& archive) {
    std::sort(objects.begin(), objects.end());
    build_step archiving;
    std::string error;
    if (!plan_archive(run.properties, objects, archive, archiving, error)) {
        return fail(run, failure_kind::recipe, error);
    }

    bool needed = true;
    std::string reason;
    if (!step_needed(run, archiving, needed, reason)) {
        return false;
    }

    if (!needed) {
        return true;
    }

    if (unlink(archive.c_str()) != 0 && errno != ENOENT) {
        return fail(run, failure_kind::io, "cannot remove " + archive + ": " + strerror(errno));
    }

    return run_step_commands(run, archiving);
}

// arduino-cli deletes each library tree in the build that no library the sketch uses names as its folder
static bool remove_unused_library_trees(build_run& run) {
    std::string root = library_builds_directory(run.directory);
    std::vector<std::string> folders;
    std::string error;
    if (is_directory(root) && !list_subdirectories(root, folders, error)) {
        return fail(run, failure_kind::io, error);
    }

    for (const std::string& folder : folders) {
        bool used = std::any_of(run.added.begin(), run.added.end(), [&](const library* added) {
            return added->folder_name == folder;
        });

        if (!used && !remove_tree(join_path(root, folder), error)) {
            return fail(run, failure_kind::io, error);
        }
    }

    return true;
}

// Each library in the order it joined: its source directory's files, then a flat library's utility/ folder's, each
// group compiling up to `jobs` at once, then its archive for `dot_a_linkage`. The stage decides once its hooks ran.
static bool compile_libraries(build_run& run) {
    const std::vector<library_compiles>& libraries = run.compiles.libraries;
    needed_compiles decided;
    std::vector<library_compiles> needed(libraries.size());
    for (size_t i = 0; i < libraries.size(); i++) {
        if (!select_needed(run, libraries[i].sources, decided, needed[i].sources) ||
            !select_needed(run, libraries[i].utility, decided, needed[i].utility)) {
            return false;
        }
    }

    settle_stage(run, LIBRARIES_STAGE, decided);
    for (size_t i = 0; i < libraries.size(); i++) {
        if (!run_compiles(run, needed[i].sources) || !run_compiles(run, needed[i].utility)) {
            return false;
        }

        const library& compiled = *libraries[i].library;
        std::string archive = library_archive(run.directory, compiled);
        if (compiled.dot_a_linkage && !archive_objects(run, objects_of(libraries[i].sources), archive)) {
            return false;
        }
    }

    return true;
}

// The core's stage once `core.prebuild` ran: the variant's compiles, the core's unless the cache holds it, and the
// final count. A hit still runs the core's hooks, as arduino-cli does, since the variant reads the file they write.
static bool compile_core(build_run& run) {
    needed_compiles decided;
    std::vector<build_step> variant_needed;
    std::vector<build_step> core_needed;
    if (!select_needed(run, run.compiles.variant, decided, variant_needed) ||
        (!run.cached.hit && !select_needed(run, run.compiles.core, decided, core_needed))) {
        return false;
    }

    settle_stage(run, CORE_STAGE, decided);
    if (!run_compiles(run, variant_needed) || !run_compiles(run, core_needed)) {
        return false;
    }

    if (compiles_needed(run) > 0) {
        run.report.progress("compile", run.compiled, compiles_needed(run), monotonic_clock_ns());
    }

    return true;
}

// A core that read nothing in the sketch folder, for a sketch shadowing none of its headers, moves into its cache
// entry, from which the build links
static bool share_core(build_run& run, const sketch& sketch) {
    const std::vector<build_step>& core = run.compiles.core;
    bool reads = true;
    std::string error;
    if (!core_reads_sketch(core, sketch.folder, true, reads, error)) {
        return fail(run, failure_kind::io, error);
    }

    std::vector<std::string> header_names;
    bool shadows = true;
    if (!reads && (!core_header_names(core, header_names, error) ||
                   !sketch_shadows(sketch.folder, header_names, shadows, error))) {
        return fail(run, failure_kind::io, error);
    }

    if (reads || shadows) {
        return true;
    }

    if (!publish_core(run.core_archive, header_names, run.cached.entry, error)) {
        return fail(run, failure_kind::io, error);
    }

    run.core_archive = join_path(run.cached.entry, CORE_ARCHIVE_NAME);
    return true;
}

// The core's archive unless the cache holds it, shared into its cache entry once made, after which the link takes the
// cache's archive
static bool archive_core(build_run& run, const sketch& sketch) {
    if (run.cached.hit) {
        run.core_archive = join_path(run.cached.entry, CORE_ARCHIVE_NAME);
        return true;
    }

    return archive_objects(run, objects_of(run.compiles.core), run.core_archive) &&
           (run.cached.entry.empty() || share_core(run, sketch));
}

// A long link's folder archives that must run, each emptied first, since `ar` adds to an archive and would keep an
// older build's members. A failing one only shows its output, as arduino-cli's linker goes on to the link.
static bool run_folder_archives(build_run& run, const std::vector<build_step>& archives) {
    for (const build_step& archive : archives) {
        bool needed = true;
        std::string reason;
        if (!step_needed(run, archive, needed, reason)) {
            return false;
        }

        if (!needed) {
            continue;
        }

        int64_t started_ns = wall_clock_ns();
        if (unlink(archive.output.c_str()) != 0 && errno != ENOENT) {
            return fail(run, failure_kind::io, "cannot remove " + archive.output + ": " + strerror(errno));
        }

        bool succeeded = true;
        for (size_t i = 0; i < archive.commands.size(); i++) {
            finished_job finished;
            if (!run_command(run, archive, i, finished)) {
                return false;
            }

            show_tool_output(run, archive.kind, finished);
            succeeded = succeeded && exit_problem(finished.exit_status).empty();
            if (cancel_requested()) {
                return fail(run, failure_kind::cancelled, "the build was cancelled");
            }
        }

        if (succeeded && !record_step(run, archive, started_ns)) {
            return false;
        }
    }

    return true;
}

// The link of the sketch's objects, then the variant's, with the core
static bool link_sketch(build_run& run, const std::string& core_archive) {
    std::vector<build_step> folder_archives;
    build_step link;
    std::string error;
    std::vector<std::string> objects = linked_objects(run.directory, run.compiles);
    if (!plan_link(run.properties, objects, core_archive, run.options.warnings, folder_archives, link, error)) {
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
    switch (stage.stage) {
    case build_stage::hooks:
        return run_hooks(run, stage.hook_point);
    case build_stage::sketch_tree:
        return prepare_sketch_tree(run, sketch);
    case build_stage::discovery:
        return discover_libraries(run, sketch, board);
    case build_stage::prototypes:
        return add_prototypes(run, sketch);
    case build_stage::sketch_compiles:
        return plan_and_count_compiles(run, sketch, board) && run_compiles(run, run.sketch_needed) &&
               run_compiles(run, run.sources_needed);
    case build_stage::core_compiles:
        return compile_core(run);
    case build_stage::library_trees:
        return remove_unused_library_trees(run);
    case build_stage::library_compiles:
        return compile_libraries(run);
    case build_stage::core_archive:
        return archive_core(run, sketch);
    case build_stage::link:
        return link_sketch(run, run.core_archive);
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
    run.core_archive = join_path(join_path(run.directory, CORE_FOLDER), CORE_ARCHIVE_NAME);
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
    file_lock& tree_lock,
    build_result& result,
    failure& failed
) {
    std::string directory = build_directory_for(layout, sketch.folder);
    std::string error;
    if (!make_directories(directory, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    if (!tree_lock.acquire(join_path(directory, BUILD_LOCK_NAME), lock_mode::exclusive, error)) {
        failed = { failure_kind::lock, error };
        return false;
    }

    job_runner runner(options.jobs);
    property_table properties = build_properties(board_properties(board, tools), sketch, directory);
    build_run run = {
        layout, options, report, failed, runner, std::move(properties), directory, {}, {}, {}, 0, 0, {}, {}, {}, false,
        sketch.main_file, !sketch.other_ino_files.empty(), false, {}, {}, {}, "",
    };

    if (!build_in_tree(run, sketch, board)) {
        return false;
    }

    std::string project = run.properties.get("build.project_name");
    std::string image = run.properties.expand(run.properties.get("recipe.output.tmp_file"));
    result.directory = directory;
    result.elf = join_path(directory, project + ".elf");
    result.image = image.empty() ? result.elf : join_path(directory, image);
    result.up_to_date = !run.ran_step;

    return true;
}
