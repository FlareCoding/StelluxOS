#include "commands.hpp"

#include "build.hpp"
#include "build_plan.hpp"
#include "cancel.hpp"
#include "cores.hpp"
#include "file_lock.hpp"
#include "files.hpp"
#include "fqbn.hpp"
#include "jobs.hpp"
#include "layout.hpp"
#include "monitor.hpp"
#include "requirements.hpp"
#include "serial_ports.hpp"
#include "sketch.hpp"
#include "tools.hpp"
#include "upload.hpp"

#include <optional>

// What a build of the sketch the command line names needs, gathered and checked before it starts
struct prepared_build {
    sketch                      found;
    sketch_settings             settings;
    file_lock                   state_lock;
    std::vector<installed_core> cores;
    selected_board              board;
    std::vector<located_tool>   tools;
};

static bool is_core_installed(const std::vector<installed_core>& cores, const board_request& request) {
    for (const installed_core& core : cores) {
        if (core.packager == request.packager && core.architecture == request.architecture) {
            return true;
        }
    }

    return false;
}

static bool fail(reporter& report, failure_kind kind, const std::string& reason) {
    report.failed({ kind, reason });
    return false;
}

// Loads the sketch and its board with the state lock held shared, so no install replaces the core meanwhile
static bool prepare_build(const arguments& parsed, const directory_layout& layout, reporter& report,
                          prepared_build& build) {
    std::string error;
    if (!load_sketch(parsed.operands.empty() ? "." : parsed.operands[0], build.found, error) ||
        !read_sketch_settings(build.found, build.settings, error)) {
        return fail(report, failure_kind::io, error);
    }

    report.sketch_found(build.found);
    std::string board_text = parsed.board.empty() ? build.settings.fqbn : parsed.board;
    if (board_text.empty()) {
        return fail(report, failure_kind::board, "no board chosen for " + build.found.name +
                                                     ". Find one with mcufw boards, then run mcufw build --board <id>");
    }

    board_request request;
    if (!parse_board_request(board_text, request, error)) {
        return fail(report, failure_kind::board, error);
    }

    if (!make_directories(layout.state, error) ||
        !build.state_lock.acquire(join_path(layout.state, STATE_LOCK_NAME), lock_mode::shared, error)) {
        return fail(report, failure_kind::lock, error);
    }

    if (!load_installed_cores(layout, build.cores, error)) {
        return fail(report, failure_kind::core, error);
    }

    if (!select_board(build.cores, request, build.board, error)) {
        if (!request.packager.empty() && !is_core_installed(build.cores, request)) {
            return fail(report, failure_kind::core, error + ". Install it with mcufw install " + board_text);
        }

        return fail(report, failure_kind::board, error);
    }

    bool saved = !parsed.board.empty();
    if (saved && !save_sketch_fqbn(build.found, requested_fqbn(build.board), error)) {
        return fail(report, failure_kind::io, error);
    }

    report.board_selected(build.board, saved);
    const installed_core& core = *build.board.core;
    if (!core.newest_overridden_version.empty()) {
        std::string name = core.packager + ":" + core.architecture;
        std::string reason = name + " " + core.version + " has no Stellux overrides in this image. mcufw install " +
                             name + " moves it to " + core.newest_overridden_version;

        return fail(report, failure_kind::core, reason);
    }

    build.tools = locate_tools(core.tools, layout);
    std::string missing = describe_missing(check_requirements(build.board, build.tools));
    if (!missing.empty()) {
        std::string fqbn = requested_fqbn(build.board);
        return fail(report, failure_kind::core, fqbn + " needs " + missing + ". Run mcufw install " + fqbn);
    }

    return true;
}

static bool build_prepared(const arguments& parsed, const directory_layout& layout, reporter& report,
                           prepared_build& build, file_lock& tree_lock, build_result& result) {
    build_options options = { parsed.jobs == 0 ? cpu_count() : parsed.jobs, parsed.warnings, parsed.verbose };
    failure failed;
    if (!build_sketch(layout, build.found, build.board, build.tools, options, report, tree_lock, result, failed)) {
        report.failed(failed);
        return false;
    }

    return true;
}

// Splits `bytes` at CR and LF into `current`, the line still open, and `last`, the last nonempty line they ended, so a
// failed upload can name what its tool said last
static void keep_last_line(std::string_view bytes, std::string& current, std::string& last) {
    for (char c : bytes) {
        if (c != '\r' && c != '\n') {
            current += c;
            continue;
        }

        if (!current.empty()) {
            last = current;
            current.clear();
        }
    }
}

// The 1200 baud touch and the waits a board asks for, around its upload recipe, naming in `answering_port` the port the
// board answers on after the upload. A failed touch only warns, as arduino-cli's reset does.
static bool upload_sketch(const arguments& parsed, reporter& report, const property_table& properties,
                          const std::string& port, const std::vector<std::string>& present,
                          std::string& answering_port) {
    uint64_t started = monotonic_clock_ns();
    bool waits = waits_for_upload_port(properties);
    std::string upload_port = port;
    std::string error;
    if (wants_1200_baud_touch(properties)) {
        if (!touch_at_1200_baud(port, error)) {
            report.tool_output("flash", "the 1200 baud touch failed: " + error + "\n");
        }

        if (waits && !wait_for_port(present, port, TOUCH_PORT_WAIT_NS, upload_port, error)) {
            return fail(report, cancel_requested() ? failure_kind::cancelled : failure_kind::port, error);
        }
    }

    std::vector<std::string> words;
    if (!upload_words(properties, upload_port, parsed.verbose, words, error)) {
        return fail(report, failure_kind::flash, error);
    }

    if (!words.empty()) {
        if (parsed.verbose) {
            report.command_started(words);
        }

        std::string current_line;
        std::string last_line;
        auto take = [&](std::string_view bytes) {
            report.upload_output(bytes);
            keep_last_line(bytes, current_line, last_line);
        };

        int exit_status = 0;
        if (!run_streamed(words, true, 0, take, exit_status, error)) {
            return fail(report, failure_kind::flash, error);
        }

        std::string problem = exit_problem(exit_status);
        if (cancel_requested()) {
            return fail(report, failure_kind::cancelled, "the flash was cancelled");
        }

        if (!problem.empty()) {
            std::string said = current_line.empty() ? last_line : current_line;
            return fail(report, failure_kind::flash, words[0] + " " + problem + (said.empty() ? "" : ": " + said));
        }
    }

    answering_port = upload_port;
    if (waits) {
        std::vector<std::string> after;
        if (!list_serial_ports(after, error) ||
            !wait_for_port(after, port, UPLOAD_PORT_WAIT_NS, answering_port, error)) {
            return fail(report, cancel_requested() ? failure_kind::cancelled : failure_kind::port, error);
        }
    }

    report.flashed(answering_port, monotonic_clock_ns() - started);

    return true;
}

int run_build(const arguments& parsed, const directory_layout& layout, reporter& report) {
    uint64_t started = monotonic_clock_ns();
    watch_for_cancel();

    prepared_build build;
    file_lock tree_lock;
    build_result result;
    if (!prepare_build(parsed, layout, report, build) ||
        !build_prepared(parsed, layout, report, build, tree_lock, result)) {
        return 1;
    }

    report.built(result, monotonic_clock_ns() - started);
    return 0;
}

int run_flash(const arguments& parsed, const directory_layout& layout, reporter& report) {
    uint64_t started = monotonic_clock_ns();
    watch_for_cancel();

    prepared_build build;
    file_lock tree_lock;
    build_result result;
    if (!prepare_build(parsed, layout, report, build) ||
        !build_prepared(parsed, layout, report, build, tree_lock, result)) {
        return 1;
    }

    report.built(result, monotonic_clock_ns() - started);

    std::vector<std::string> present;
    std::string port;
    std::string why;
    std::string error;
    failure failed;
    if (!list_serial_ports(present, error)) {
        report.failed({ failure_kind::port, error });
        return 1;
    }

    if (!choose_serial_port(parsed.port, build.settings.port, present, port, why, failed)) {
        report.failed(failed);
        return 1;
    }

    report.port_chosen(port, why);
    property_table properties = build_properties(board_properties(build.board, build.tools), build.found,
                                                 result.directory);
    std::string answering_port;
    if (!upload_sketch(parsed, report, properties, port, present, answering_port)) {
        return 1;
    }

    if (!save_sketch_port(build.found, parsed.port, port, error)) {
        report.failed({ failure_kind::io, error });
        return 1;
    }

    if (!parsed.monitor) {
        return 0;
    }

    tree_lock.release();
    build.state_lock.release();
    monitor_request request = board_monitor_request(answering_port, parsed.baud, properties, parsed.serial_options);
    std::optional<crash_decoder> decoder;
    prepare_crash_decoding(properties, report, decoder, request);

    int exit_status = 0;
    if (!run_monitor(request, report, exit_status, failed)) {
        report.failed(failed);
        return 1;
    }

    return exit_status;
}
