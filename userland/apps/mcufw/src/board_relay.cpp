#include "board_relay.hpp"

#include "elf.hpp"
#include "files.hpp"

#include <algorithm>
#include <utility>

std::string addr2line_beside(const std::string& compiler) {
    std::string name = file_name(compiler);
    size_t gcc = name.rfind("gcc");
    if (compiler.empty() || gcc == std::string::npos) {
        return {};
    }

    return join_path(parent_directory(compiler), name.substr(0, gcc) + "addr2line" + name.substr(gcc + 3));
}

board_relay::board_relay(crash_decoder& decoder, file_stamp elf, address_lookup look_up, reporter& report)
    : m_decoder(decoder), m_elf(std::move(elf)), m_look_up(std::move(look_up)), m_report(report) {}

// Reads the ELF again once a build replaced it, which also starts decoding again after a mismatch or failed read
void board_relay::reload_rebuilt_elf() {
    file_stamp now;
    bool is_unchanged = stamp_file(m_elf.path, false, now) && now.size == m_elf.size &&
                        now.modified_ns == m_elf.modified_ns;
    if (is_unchanged) {
        return;
    }

    bool was_decoding = m_decoder.decoding();
    std::vector<address_range> ranges;
    std::string error;
    if (!stamp_file(m_elf.path, true, now) || !read_executable_ranges(m_elf.path, ranges, error)) {
        // addr2line would read the new file against the old ranges, so decoding stops until a read succeeds
        m_decoder.stop();
        if (was_decoding) {
            m_report.decoding_off(error.empty() ? "cannot read " + m_elf.path : error);
        }

        return;
    }

    m_elf = now;
    m_decoder.reload(std::move(ranges), now.digest);
    if (!was_decoding) {
        m_report.decoding(m_elf.path);
    }
}

void board_relay::print_ready_lines() {
    std::vector<std::string> ready = m_decoder.take_ready_lines();
    if (!ready.empty()) {
        m_report.decoded_lines(ready);
    }
}

// Every line looks for a rebuilt ELF first, since a crash at addresses already looked up asks for no lookup
void board_relay::decode_line() {
    reload_rebuilt_elf();
    bool was_decoding = m_decoder.decoding();
    std::vector<uint32_t> addresses = m_decoder.take_line(m_line);
    if (was_decoding && !m_decoder.decoding()) {
        m_report.decoding_off("the board runs another build than " + m_elf.path);
    }

    if (!addresses.empty()) {
        m_decoder.take_lookup(addresses, m_look_up(m_elf.path, addresses));
    }

    print_ready_lines();
}

void board_relay::take_bytes(std::string_view bytes) {
    if (bytes.empty()) {
        m_decoder.release_held();
        print_ready_lines();
        return;
    }

    while (!bytes.empty()) {
        size_t end = bytes.find('\n');
        bool ends_line = end != std::string_view::npos;
        std::string_view piece = bytes.substr(0, ends_line ? end + 1 : bytes.size());
        m_report.board_output(piece);
        bytes.remove_prefix(piece.size());

        std::string_view text = ends_line ? piece.substr(0, piece.size() - 1) : piece;
        m_line.append(text.substr(0, LINE_BYTES_MAX - std::min(m_line.size(), LINE_BYTES_MAX)));
        if (ends_line) {
            decode_line();
            m_line.clear();
        }
    }
}
