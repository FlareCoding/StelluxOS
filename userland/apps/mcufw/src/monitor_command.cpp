#include "commands.hpp"

#include "boards.hpp"
#include "build_plan.hpp"
#include "cancel.hpp"
#include "cores.hpp"
#include "file_lock.hpp"
#include "files.hpp"
#include "fqbn.hpp"
#include "layout.hpp"
#include "monitor.hpp"
#include "serial_ports.hpp"
#include "sketch.hpp"
#include "tools.hpp"

#include <optional>

static int fail(reporter& report, failure_kind kind, const std::string& reason) {
    report.failed({ kind, reason });
    return 1;
}

// The properties of the board sketch.yaml names, read with the state lock held only until they load, since a monitor
// session can last hours
static bool load_board_properties(const directory_layout& layout, const std::string& fqbn, property_table& properties,
                                  failure& failed) {
    board_request wanted;
    std::string error;
    if (!parse_board_request(fqbn, wanted, error)) {
        failed = { failure_kind::board, error };
        return false;
    }

    file_lock state_lock;
    if (!make_directories(layout.state, error) ||
        !state_lock.acquire(join_path(layout.state, STATE_LOCK_NAME), lock_mode::shared, error)) {
        failed = { failure_kind::lock, error };
        return false;
    }

    std::vector<installed_core> cores;
    selected_board board;
    if (!load_installed_cores(layout, cores, error) || !select_board(cores, wanted, board, error)) {
        failed = { failure_kind::board, "the board in sketch.yaml, " + fqbn + ", cannot be used: " + error };
        return false;
    }

    properties = board_properties(board, locate_tools(board.core->tools, layout));

    return true;
}

int run_monitor_command(const arguments& parsed, const directory_layout& layout, reporter& report) {
    watch_for_cancel();

    sketch found;
    sketch_settings settings;
    std::string error;
    bool in_sketch = load_sketch(parsed.operands.empty() ? "." : parsed.operands[0], found, error);
    if (!in_sketch && !parsed.operands.empty()) {
        return fail(report, failure_kind::io, error);
    }

    if (in_sketch && !read_sketch_settings(found, settings, error)) {
        return fail(report, failure_kind::io, error);
    }

    property_table properties;
    failure failed;
    if (!settings.fqbn.empty() && !load_board_properties(layout, settings.fqbn, properties, failed)) {
        report.failed(failed);
        return 1;
    }

    std::vector<std::string> present;
    std::string port;
    std::string why;
    if (!list_serial_ports(present, error)) {
        return fail(report, failure_kind::port, error);
    }

    if (!choose_serial_port(parsed.port, settings.port, present, port, why, failed)) {
        report.failed(failed);
        return 1;
    }

    if (in_sketch && !save_sketch_port(found, parsed.port, port, error)) {
        return fail(report, failure_kind::io, error);
    }

    monitor_request request = board_monitor_request(port, parsed.baud, properties, parsed.serial_options);
    std::optional<crash_decoder> decoder;
    if (in_sketch && settings.fqbn.empty()) {
        report.decoding_off("the sketch has no board in sketch.yaml");
    } else if (in_sketch) {
        std::string build_directory = build_directory_for(layout, found.folder);
        prepare_crash_decoding(build_properties(properties, found, build_directory), report, decoder, request);
    }

    int exit_status = 0;
    if (!run_monitor(request, report, exit_status, failed)) {
        report.failed(failed);
        return 1;
    }

    return exit_status;
}
