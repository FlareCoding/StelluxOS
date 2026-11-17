#include "monitor.hpp"

#include "board_relay.hpp"
#include "elf.hpp"
#include "files.hpp"
#include "jobs.hpp"

#include <stlx/proc.h>

#include <cstdio>

void prepare_crash_decoding(const property_table& build_properties, reporter& report,
                            std::optional<crash_decoder>& decoder, monitor_request& request) {
    std::string elf = join_path(build_properties.get("build.path"),
                                build_properties.get("build.project_name") + ".elf");
    if (!is_file(elf)) {
        report.decoding_off("the sketch has not been built");
        return;
    }

    std::string addr2line = addr2line_beside(find_program(build_properties.expand("{compiler.path}{compiler.c.cmd}")));
    if (addr2line.empty() || !is_file(addr2line)) {
        report.decoding_off("the toolchain has no addr2line");
        return;
    }

    file_stamp stamp;
    std::vector<address_range> ranges;
    std::string error;
    if (!stamp_file(elf, true, stamp) || !read_executable_ranges(elf, ranges, error)) {
        report.decoding_off(error.empty() ? "cannot read " + elf : error);
        return;
    }

    decoder.emplace(std::move(ranges), stamp.digest);
    request.decoder = &*decoder;
    request.elf = stamp;
    request.addr2line = addr2line;
    report.decoding(elf);
}

// addr2line's output for `addresses`, empty when it fails, which leaves them undecoded for the session
static std::string look_up_addresses(const std::string& addr2line, const std::string& elf,
                                     const std::vector<uint32_t>& addresses) {
    job lookup = { 1, { addr2line, "-fiaC", "-e", elf }, "" };
    for (uint32_t address : addresses) {
        char text[sizeof("0x00000000")];
        snprintf(text, sizeof(text), "0x%08x", address);
        lookup.words.emplace_back(text);
    }

    job_runner runner(1);
    finished_job finished;
    std::string error;
    bool looked_up = runner.start(lookup, error) && runner.wait_for_next(finished, error) &&
                     exit_problem(finished.exit_status).empty();

    return looked_up ? finished.output : std::string();
}

bool run_monitor(const monitor_request& request, reporter& report, int& exit_status, failure& failed) {
    std::vector<std::string> words = serial_monitor_words(request);
    int wait_status = 0;
    std::string error;
    bool ran = false;
    if (request.decoder) {
        auto look_up = [&request](const std::string& elf, const std::vector<uint32_t>& addresses) {
            return look_up_addresses(request.addr2line, elf, addresses);
        };

        board_relay relay(*request.decoder, request.elf, look_up, report);
        auto take = [&relay](std::string_view bytes) { relay.take_bytes(bytes); };
        ran = run_streamed(words, false, SILENCE_BEFORE_RELEASE_NS, take, wait_status, error);
        relay.take_bytes({});
    } else {
        ran = run_attached(words, wait_status, error);
    }

    if (!ran) {
        failed = { failure_kind::port, error };
        return false;
    }

    exit_status = STLX_WIFEXITED(wait_status) ? STLX_WEXITSTATUS(wait_status) : 1;

    return true;
}
