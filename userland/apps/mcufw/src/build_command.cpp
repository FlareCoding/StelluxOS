#include "commands.hpp"

#include "build.hpp"
#include "cancel.hpp"
#include "cores.hpp"
#include "file_lock.hpp"
#include "files.hpp"
#include "fqbn.hpp"
#include "jobs.hpp"
#include "layout.hpp"
#include "requirements.hpp"
#include "sketch.hpp"
#include "tools.hpp"

static bool is_core_installed(const std::vector<installed_core>& cores, const board_request& request) {
    for (const installed_core& core : cores) {
        if (core.packager == request.packager && core.architecture == request.architecture) {
            return true;
        }
    }

    return false;
}

static int fail(reporter& report, failure_kind kind, const std::string& reason) {
    report.failed({ kind, reason });
    return 1;
}

int run_build(const arguments& parsed, const directory_layout& layout, reporter& report) {
    uint64_t started = monotonic_clock_ns();
    watch_for_cancel();

    sketch found;
    sketch_settings settings;
    std::string error;
    if (!load_sketch(parsed.operands.empty() ? "." : parsed.operands[0], found, error) ||
        !read_sketch_settings(found, settings, error)) {
        return fail(report, failure_kind::io, error);
    }

    report.sketch_found(found);
    std::string board_text = parsed.board.empty() ? settings.fqbn : parsed.board;
    if (board_text.empty()) {
        return fail(report, failure_kind::board, "no board chosen for " + found.name +
                                                     ". Find one with mcufw boards, then run mcufw build --board <id>");
    }

    board_request request;
    if (!parse_board_request(board_text, request, error)) {
        return fail(report, failure_kind::board, error);
    }

    file_lock state_lock;
    if (!make_directories(layout.state, error) ||
        !state_lock.acquire(join_path(layout.state, STATE_LOCK_NAME), lock_mode::shared, error)) {
        return fail(report, failure_kind::lock, error);
    }

    std::vector<installed_core> cores;
    if (!load_installed_cores(layout, cores, error)) {
        return fail(report, failure_kind::core, error);
    }

    selected_board board;
    if (!select_board(cores, request, board, error)) {
        if (!request.packager.empty() && !is_core_installed(cores, request)) {
            return fail(report, failure_kind::core, error + ". Install it with mcufw install " + board_text);
        }

        return fail(report, failure_kind::board, error);
    }

    bool saved = !parsed.board.empty();
    if (saved && !save_sketch_fqbn(found, requested_fqbn(board), error)) {
        return fail(report, failure_kind::io, error);
    }

    report.board_selected(board, saved);
    const installed_core& core = *board.core;
    if (!core.newest_overridden_version.empty()) {
        std::string name = core.packager + ":" + core.architecture;
        std::string reason = name + " " + core.version + " has no Stellux overrides in this image. mcufw install " +
                             name + " moves it to " + core.newest_overridden_version;

        return fail(report, failure_kind::core, reason);
    }

    std::vector<located_tool> tools = locate_tools(core.tools, layout);
    std::string missing = describe_missing(check_requirements(board, tools));
    if (!missing.empty()) {
        std::string fqbn = requested_fqbn(board);
        return fail(report, failure_kind::core, fqbn + " needs " + missing + ". Run mcufw install " + fqbn);
    }

    build_options options = { parsed.jobs == 0 ? cpu_count() : parsed.jobs, parsed.warnings, parsed.verbose };
    build_result result;
    failure failed;
    if (!build_sketch(layout, found, board, tools, options, report, result, failed)) {
        report.failed(failed);
        return 1;
    }

    report.built(result, monotonic_clock_ns() - started);
    return 0;
}
