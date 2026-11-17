#include "crash_decoder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

constexpr std::string_view HASH_LINE_LABEL        = "ELF file SHA256:";
constexpr std::string_view UNKNOWN_FUNCTION       = "??";
constexpr std::string_view UNKNOWN_LOCATION_START = "??:";
constexpr std::string_view HEX_PREFIX             = "0x";
constexpr size_t           ADDRESS_DIGITS         = 8;

// One address as addr2line placed it, its frames as function and location lines in turn
struct addr2line_entry {
    std::string              address_text;
    std::vector<std::string> frame_lines;
};

static bool is_hex_digit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

static bool is_in_ranges(uint32_t address, const std::vector<address_range>& ranges) {
    for (const address_range& range : ranges) {
        if (address >= range.start && address < range.end) {
            return true;
        }
    }

    return false;
}

static uint32_t parse_hex(std::string_view digits) {
    return static_cast<uint32_t>(strtoul(std::string(digits).c_str(), nullptr, 16));
}

static bool is_address_line(std::string_view line) {
    return line.size() > HEX_PREFIX.size() && line.starts_with(HEX_PREFIX) &&
           std::all_of(line.begin() + HEX_PREFIX.size(), line.end(), is_hex_digit);
}

static std::vector<addr2line_entry> read_addr2line_entries(std::string_view output) {
    std::vector<addr2line_entry> entries;
    while (!output.empty()) {
        size_t end = output.find('\n');
        std::string_view line = output.substr(0, end);
        output = end == std::string_view::npos ? std::string_view() : output.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (is_address_line(line)) {
            entries.push_back({ std::string(line), {} });
        } else if (!entries.empty()) {
            entries.back().frame_lines.emplace_back(line);
        }
    }

    return entries;
}

static std::vector<std::string> describe_entry(const addr2line_entry& entry) {
    const std::vector<std::string>& frames = entry.frame_lines;
    if (frames.size() < 2) {
        return {};
    }

    bool is_unplaced = frames[0] == UNKNOWN_FUNCTION && frames[1].starts_with(UNKNOWN_LOCATION_START);
    if (is_unplaced) {
        return {};
    }

    std::vector<std::string> lines;
    for (size_t frame = 0; frame + 1 < frames.size(); frame += 2) {
        std::string place = frames[frame] + " at " + frames[frame + 1];
        lines.push_back(frame == 0 ? entry.address_text + ": " + place : " (inlined by) " + place);
    }

    return lines;
}

// The hex digits after the label of a hash line, lowercased, or false for any other line
static bool find_hash_digits(std::string_view line, std::string& digits) {
    size_t label = line.find(HASH_LINE_LABEL);
    if (label == std::string_view::npos) {
        return false;
    }

    size_t at = label + HASH_LINE_LABEL.size();
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
        at++;
    }

    digits.clear();
    for (; at < line.size() && is_hex_digit(line[at]); at++) {
        digits += static_cast<char>(std::tolower(static_cast<unsigned char>(line[at])));
    }

    return !digits.empty();
}

std::vector<uint32_t> find_code_addresses(std::string_view line, const std::vector<address_range>& ranges) {
    std::vector<uint32_t> addresses;
    size_t at = 0;
    while (at + HEX_PREFIX.size() + ADDRESS_DIGITS <= line.size()) {
        std::string_view digits = line.substr(at + HEX_PREFIX.size(), ADDRESS_DIGITS);
        bool starts_address = line[at] == '0' && (line[at + 1] == 'x' || line[at + 1] == 'X') &&
                              std::all_of(digits.begin(), digits.end(), is_hex_digit);
        if (!starts_address) {
            at++;
            continue;
        }

        uint32_t address = parse_hex(digits);
        if (is_in_ranges(address, ranges)) {
            addresses.push_back(address);
        }

        at += HEX_PREFIX.size() + ADDRESS_DIGITS;
    }

    return addresses;
}

std::vector<std::string> format_addr2line_output(std::string_view output) {
    std::vector<std::string> lines;
    for (const addr2line_entry& entry : read_addr2line_entries(output)) {
        std::vector<std::string> described = describe_entry(entry);
        lines.insert(lines.end(), described.begin(), described.end());
    }

    return lines;
}

crash_decoder::crash_decoder(std::vector<address_range> ranges, std::string elf_sha256)
    : m_ranges(std::move(ranges)), m_elf_sha256(std::move(elf_sha256)) {}

std::vector<uint32_t> crash_decoder::take_line(std::string_view line) {
    if (!m_decoding) {
        return {};
    }

    std::string digits;
    if (find_hash_digits(line, digits)) {
        bool is_same_build = digits.size() <= m_elf_sha256.size() &&
                             m_elf_sha256.compare(0, digits.size(), digits) == 0;
        if (is_same_build) {
            m_ready.insert(m_ready.end(), m_held.begin(), m_held.end());
        } else {
            m_decoding = false;
        }

        m_held.clear();
        m_has_seen_hash_line = true;
        return {};
    }

    m_pending = find_code_addresses(line, m_ranges);
    std::vector<uint32_t> unknown;
    for (uint32_t address : m_pending) {
        bool is_unknown = m_decoded.count(address) == 0 &&
                          std::find(unknown.begin(), unknown.end(), address) == unknown.end();
        if (is_unknown) {
            unknown.push_back(address);
        }
    }

    if (unknown.empty()) {
        hold_pending_lines();
    }

    return unknown;
}

void crash_decoder::take_lookup(const std::vector<uint32_t>& addresses, std::string_view addr2line_output) {
    for (uint32_t address : addresses) {
        m_decoded[address].clear();
    }

    for (const addr2line_entry& entry : read_addr2line_entries(addr2line_output)) {
        m_decoded[parse_hex(std::string_view(entry.address_text).substr(HEX_PREFIX.size()))] = describe_entry(entry);
    }

    hold_pending_lines();
}

void crash_decoder::release_held() {
    if (!m_has_seen_hash_line) {
        m_ready.insert(m_ready.end(), m_held.begin(), m_held.end());
    }

    m_held.clear();
}

void crash_decoder::reload(std::vector<address_range> ranges, std::string elf_sha256) {
    m_ranges = std::move(ranges);
    m_elf_sha256 = std::move(elf_sha256);
    m_decoded.clear();
    m_pending.clear();
    m_held.clear();
    m_decoding = true;
}

void crash_decoder::stop() {
    m_pending.clear();
    m_held.clear();
    m_decoding = false;
}

std::vector<std::string> crash_decoder::take_ready_lines() {
    std::vector<std::string> ready;
    ready.swap(m_ready);
    return ready;
}

bool crash_decoder::decoding() const {
    return m_decoding;
}

void crash_decoder::hold_pending_lines() {
    for (uint32_t address : m_pending) {
        auto decoded = m_decoded.find(address);
        if (decoded != m_decoded.end()) {
            m_held.insert(m_held.end(), decoded->second.begin(), decoded->second.end());
        }
    }

    m_pending.clear();
}
