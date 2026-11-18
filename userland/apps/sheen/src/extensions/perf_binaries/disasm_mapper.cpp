#include "extensions/perf_binaries/disasm_mapper.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <map>

namespace sheen {

static constexpr size_t MAX_HEX_ADDR_DIGITS = 16;
static constexpr size_t HEX_BYTE_DIGITS = 2;
static constexpr size_t HEX_WORD32_DIGITS = 8;
static constexpr size_t MIN_COLUMN_GAP_SPACES = 2;
static constexpr uint8_t PRINTABLE_ASCII_MIN = 0x20;
static constexpr uint8_t PRINTABLE_ASCII_MAX = 0x7e;
static constexpr uint64_t NS_PER_MS = 1000000ull;
static constexpr uint64_t NS_PER_US = 1000ull;
static constexpr double PERCENT_SCALE = 100.0;

static constexpr std::string_view DISCRIMINATOR_TAG = " (discriminator ";
static constexpr std::string_view UNKNOWN_SOURCE_FILE = "??";
static constexpr std::string_view DISASM_SECTION_PREFIX = "Disassembly of section ";
static constexpr std::string_view FILE_FORMAT_PREFIX = "file format ";

namespace {

struct indexed_instruction_bounds {
    uint64_t start = 0;
    uint64_t end = 0;
    size_t row_index = 0;
};

} // namespace

static bool is_ascii_hex(char ch) {
    return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

static bool is_ascii_digit(char ch) {
    return std::isdigit(static_cast<unsigned char>(ch)) != 0;
}

static uint8_t hex_nibble_value(char ch) {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (uch >= '0' && uch <= '9') {
        return static_cast<uint8_t>(uch - '0');
    }

    return static_cast<uint8_t>(10 + (std::tolower(uch) - 'a'));
}

static std::string_view trim_left_ws(std::string_view text) {
    size_t pos = 0;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        pos++;
    }

    return text.substr(pos);
}

static std::string_view trim_right_ws(std::string_view text) {
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        end--;
    }

    return text.substr(0, end);
}

static std::string_view trim_ws(std::string_view text) {
    return trim_left_ws(trim_right_ws(text));
}

static std::string sanitize_disasm_text(std::string_view raw) {
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (size_t idx = 0; idx < raw.size();) {
        uint8_t lead = static_cast<uint8_t>(raw[idx]);
        if (lead >= PRINTABLE_ASCII_MIN && lead <= PRINTABLE_ASCII_MAX) {
            cleaned += static_cast<char>(lead);
            idx++;
            continue;
        }

        size_t seq = (lead & 0xe0u) == 0xc0u ? 2 : ((lead & 0xf0u) == 0xe0u ? 3 : ((lead & 0xf8u) == 0xf0u ? 4 : 0));
        uint32_t cp = seq == 2 ? (lead & 0x1fu) : (seq == 3 ? (lead & 0x0fu) : (lead & 0x07u));
        uint32_t min_cp = seq == 2 ? 0x80u : (seq == 3 ? 0x800u : 0x10000u);
        bool valid = seq > 0 && idx + seq <= raw.size();
        for (size_t k = 1; valid && k < seq; k++) {
            uint8_t cont = static_cast<uint8_t>(raw[idx + k]);
            valid = (cont & 0xc0u) == 0x80u;
            cp = (cp << 6) | (cont & 0x3fu);
        }

        if (valid && cp >= min_cp && cp <= 0x10ffffu && (cp < 0xd800u || cp > 0xdfffu)) {
            cleaned.append(raw.data() + idx, seq);
            idx += seq;
        } else {
            cleaned += '?';
            idx++;
        }
    }

    return cleaned;
}

static bool is_file_format_banner(std::string_view trimmed) {
    size_t colon = trimmed.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }

    std::string_view after = trim_left_ws(trimmed.substr(colon + 1));
    return after.starts_with(FILE_FORMAT_PREFIX);
}

static bool parse_hex_u64(std::string_view digits, uint64_t& value) {
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
    }

    if (digits.empty()) {
        return false;
    }

    while (digits.size() > 1 && digits.front() == '0') {
        digits.remove_prefix(1);
    }

    if (digits.size() > MAX_HEX_ADDR_DIGITS) {
        return false;
    }

    uint64_t acc = 0;
    for (char ch : digits) {
        if (!is_ascii_hex(ch)) {
            return false;
        }

        acc = (acc << 4) | hex_nibble_value(ch);
    }

    value = acc;
    return true;
}

static uint64_t saturating_add_u64(uint64_t lhs, uint64_t rhs) {
    return rhs > UINT64_MAX - lhs ? UINT64_MAX : (lhs + rhs);
}

static bool try_parse_symbol_header(std::string_view raw_line, uint64_t& address, std::string& symbol_name) {
    if (raw_line.empty() || raw_line.front() == ' ' || raw_line.front() == '\t') {
        return false;
    }

    std::string_view trimmed = trim_right_ws(raw_line);
    if (!trimmed.ends_with(">:")) {
        return false;
    }

    size_t space_pos = trimmed.find(' ');
    if (space_pos == std::string_view::npos || space_pos + 2 >= trimmed.size() || trimmed[space_pos + 1] != '<') {
        return false;
    }

    if (!parse_hex_u64(trimmed.substr(0, space_pos), address)) {
        return false;
    }

    size_t name_start = space_pos + 2;
    size_t name_len = (trimmed.size() - 2) - name_start;
    if (name_len == 0) {
        return false;
    }

    symbol_name = sanitize_disasm_text(trimmed.substr(name_start, name_len));
    return true;
}

static std::string_view strip_discriminator_suffix(std::string_view line) {
    if (!line.ends_with(')')) {
        return line;
    }

    size_t tag_pos = line.rfind(DISCRIMINATOR_TAG);
    if (tag_pos == std::string_view::npos) {
        return line;
    }

    std::string_view digits = line.substr(tag_pos + DISCRIMINATOR_TAG.size(),
                                          line.size() - 1 - (tag_pos + DISCRIMINATOR_TAG.size()));
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), is_ascii_digit)) {
        return line;
    }

    return line.substr(0, tag_pos);
}

static bool try_parse_source_marker(std::string_view raw_line, std::string& source_file,
                                    int& source_line, bool& clears_location) {
    clears_location = false;
    std::string_view text = trim_ws(raw_line);
    if (text.starts_with(';')) {
        text = trim_left_ws(text.substr(1));
    }

    text = strip_discriminator_suffix(text);
    size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }

    std::string_view file_part = text.substr(0, colon);
    std::string_view line_part = text.substr(colon + 1);
    if (file_part.find('\t') != std::string_view::npos || file_part.back() == ' ') {
        return false;
    }

    if (file_part == UNKNOWN_SOURCE_FILE && (line_part == "0" || line_part == "?")) {
        clears_location = true;
        return true;
    }

    if (!std::all_of(line_part.begin(), line_part.end(), is_ascii_digit)) {
        return false;
    }

    uint64_t parsed_line = 0;
    for (char ch : line_part) {
        parsed_line = parsed_line * 10 + static_cast<uint64_t>(ch - '0');
        if (parsed_line > static_cast<uint64_t>(INT_MAX)) {
            return false;
        }
    }

    if (parsed_line == 0) {
        clears_location = true;
        return true;
    }

    source_file = sanitize_disasm_text(file_part);
    source_line = static_cast<int>(parsed_line);
    return true;
}

static bool parse_raw_bytes_tokens(std::string_view column, std::vector<uint8_t>& out_bytes,
                                   std::string& out_normalized_text) {
    std::string_view trimmed = trim_ws(column);
    if (trimmed.empty()) {
        return false;
    }

    if (trimmed.size() == HEX_WORD32_DIGITS && std::all_of(trimmed.begin(), trimmed.end(), is_ascii_hex)) {
        uint64_t word = 0;
        if (!parse_hex_u64(trimmed, word)) {
            return false;
        }

        out_bytes = {
            static_cast<uint8_t>(word & 0xffu),
            static_cast<uint8_t>((word >> 8) & 0xffu),
            static_cast<uint8_t>((word >> 16) & 0xffu),
            static_cast<uint8_t>((word >> 24) & 0xffu),
        };

        out_normalized_text.clear();
        out_normalized_text.reserve(HEX_WORD32_DIGITS);
        for (char ch : trimmed) {
            out_normalized_text += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        return true;
    }

    std::vector<uint8_t> parsed;
    std::string formatted;
    size_t pos = 0;
    while (pos < trimmed.size()) {
        while (pos < trimmed.size() && trimmed[pos] == ' ') {
            pos++;
        }

        if (pos >= trimmed.size()) {
            break;
        }

        if (pos + HEX_BYTE_DIGITS > trimmed.size() || !is_ascii_hex(trimmed[pos]) || !is_ascii_hex(trimmed[pos + 1])) {
            return false;
        }

        if (pos + HEX_BYTE_DIGITS < trimmed.size() && trimmed[pos + HEX_BYTE_DIGITS] != ' ') {
            return false;
        }

        uint8_t byte_value = static_cast<uint8_t>((hex_nibble_value(trimmed[pos]) << 4) |
                                                  hex_nibble_value(trimmed[pos + 1]));
        parsed.push_back(byte_value);
        if (!formatted.empty()) {
            formatted += ' ';
        }

        formatted += static_cast<char>(std::tolower(static_cast<unsigned char>(trimmed[pos])));
        formatted += static_cast<char>(std::tolower(static_cast<unsigned char>(trimmed[pos + 1])));
        pos += HEX_BYTE_DIGITS;
    }

    if (parsed.empty()) {
        return false;
    }

    out_bytes = std::move(parsed);
    out_normalized_text = std::move(formatted);
    return true;
}

static std::string normalize_mnemonic_and_operands(std::string_view raw_insn) {
    std::string_view trimmed = trim_ws(raw_insn);
    if (trimmed.empty()) {
        return {};
    }

    size_t split = 0;
    while (split < trimmed.size() && trimmed[split] != ' ' && trimmed[split] != '\t') {
        split++;
    }

    std::string result = sanitize_disasm_text(trimmed.substr(0, split));
    std::string_view operands = trim_left_ws(trimmed.substr(split));
    if (operands.empty()) {
        return result;
    }

    result += ' ';
    std::string clean_ops = sanitize_disasm_text(operands);
    for (char ch : clean_ops) {
        result += ch == '\t' ? ' ' : ch;
    }

    return result;
}

static bool try_parse_instruction_or_continuation(std::string_view raw_line, uint64_t& address,
                                                  std::vector<uint8_t>& raw_bytes, std::string& raw_bytes_text,
                                                  std::string& mnemonic_and_operands) {
    std::string_view after_indent = trim_left_ws(trim_right_ws(raw_line));
    size_t colon = after_indent.find(':');
    if (colon == std::string_view::npos || colon == 0 || !parse_hex_u64(after_indent.substr(0, colon), address)) {
        return false;
    }

    std::string_view rest = after_indent.substr(colon + 1);
    if (rest.empty() || (rest.front() != '\t' && rest.front() != ' ')) {
        return false;
    }

    raw_bytes.clear();
    raw_bytes_text.clear();
    mnemonic_and_operands.clear();

    size_t first_tab = rest.find('\t');
    if (first_tab != std::string_view::npos) {
        if (first_tab > 0 && parse_raw_bytes_tokens(rest.substr(0, first_tab), raw_bytes, raw_bytes_text)) {
            mnemonic_and_operands = normalize_mnemonic_and_operands(rest.substr(first_tab + 1));
            return true;
        }

        std::string_view after_first_tab = rest.substr(first_tab + 1);
        size_t second_tab = after_first_tab.find('\t');
        if (second_tab == std::string_view::npos) {
            if (parse_raw_bytes_tokens(after_first_tab, raw_bytes, raw_bytes_text)) {
                return true;
            }

            mnemonic_and_operands = normalize_mnemonic_and_operands(after_first_tab);
            return !mnemonic_and_operands.empty();
        }

        std::string_view first_col = after_first_tab.substr(0, second_tab);
        bool looks_like_unpadded_2char_mnemonic = trim_left_ws(first_col).size() == HEX_BYTE_DIGITS;
        if (!looks_like_unpadded_2char_mnemonic &&
            parse_raw_bytes_tokens(first_col, raw_bytes, raw_bytes_text)) {
            mnemonic_and_operands = normalize_mnemonic_and_operands(after_first_tab.substr(second_tab + 1));
            return true;
        }

        mnemonic_and_operands = normalize_mnemonic_and_operands(after_first_tab);
        return !mnemonic_and_operands.empty();
    }

    std::string_view body = trim_left_ws(rest);
    if (body.empty()) {
        return false;
    }

    if (body.size() >= HEX_WORD32_DIGITS &&
        std::all_of(body.begin(), body.begin() + HEX_WORD32_DIGITS, is_ascii_hex) &&
        (body.size() == HEX_WORD32_DIGITS || body[HEX_WORD32_DIGITS] == ' ')) {
        parse_raw_bytes_tokens(body.substr(0, HEX_WORD32_DIGITS), raw_bytes, raw_bytes_text);
        mnemonic_and_operands = normalize_mnemonic_and_operands(body.substr(HEX_WORD32_DIGITS));
        return true;
    }

    size_t pos = 0;
    size_t last_byte_end = 0;
    while (pos + HEX_BYTE_DIGITS <= body.size() && is_ascii_hex(body[pos]) && is_ascii_hex(body[pos + 1]) &&
           (pos + HEX_BYTE_DIGITS == body.size() || body[pos + HEX_BYTE_DIGITS] == ' ')) {
        last_byte_end = pos + HEX_BYTE_DIGITS;
        pos += HEX_BYTE_DIGITS;

        size_t space_run = 0;
        while (pos + space_run < body.size() && body[pos + space_run] == ' ') {
            space_run++;
        }

        pos += space_run;
        if (space_run >= MIN_COLUMN_GAP_SPACES) {
            break;
        }
    }

    if (last_byte_end > 0 && parse_raw_bytes_tokens(body.substr(0, last_byte_end), raw_bytes, raw_bytes_text)) {
        mnemonic_and_operands = normalize_mnemonic_and_operands(body.substr(pos));
        return true;
    }

    mnemonic_and_operands = normalize_mnemonic_and_operands(body);
    return !mnemonic_and_operands.empty();
}

static std::string normalize_lexical_path(std::string_view path) {
    if (path.empty()) {
        return {};
    }

    bool is_absolute = path.front() == '/';
    std::vector<std::string_view> components;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t slash = path.find('/', pos);
        std::string_view segment = slash == std::string_view::npos ? path.substr(pos) : path.substr(pos, slash - pos);
        pos = slash == std::string_view::npos ? path.size() : slash + 1;
        if (segment.empty() || segment == ".") {
            continue;
        }

        if (segment == "..") {
            if (!components.empty() && components.back() != "..") {
                components.pop_back();
            } else if (!is_absolute) {
                components.push_back(segment);
            }

            continue;
        }

        components.push_back(segment);
    }

    std::string normalized = is_absolute ? "/" : "";
    for (size_t i = 0; i < components.size(); i++) {
        if (i > 0) {
            normalized += '/';
        }

        normalized.append(components[i]);
    }

    return normalized;
}

static void build_line_mapping_tables(disasm_map& mapped) {
    mapped.line_mappings.clear();
    mapped.instruction_to_mapping.assign(mapped.instructions.size(), UNMAPPED_DISASM_INDEX);
    mapped.primary_source_file.clear();

    std::map<std::string, std::pair<size_t, size_t>> counts_and_first_row;
    std::map<std::pair<std::string, int>, size_t> index_by_key;

    for (size_t row_idx = 0; row_idx < mapped.instructions.size(); row_idx++) {
        const disasm_instruction_row& row = mapped.instructions[row_idx];
        if (row.source_line <= 0 || row.source_file.empty()) {
            continue;
        }

        auto [file_entry, inserted] = counts_and_first_row.try_emplace(row.source_file, std::make_pair(0u, row_idx));
        (void)inserted;
        file_entry->second.first++;
        index_by_key.try_emplace(std::make_pair(row.source_file, row.source_line), 0u);
    }

    size_t best_count = 0;
    size_t earliest_row = UNMAPPED_DISASM_INDEX;
    for (const auto& [file_name, stats] : counts_and_first_row) {
        if (stats.first > best_count || (stats.first == best_count && stats.second < earliest_row)) {
            best_count = stats.first;
            earliest_row = stats.second;
            mapped.primary_source_file = file_name;
        }
    }

    mapped.line_mappings.reserve(index_by_key.size());
    for (auto& [key, assigned_idx] : index_by_key) {
        assigned_idx = mapped.line_mappings.size();
        mapped.line_mappings.push_back({ key.first, key.second, {}, 0 });
    }

    for (size_t row_idx = 0; row_idx < mapped.instructions.size(); row_idx++) {
        const disasm_instruction_row& row = mapped.instructions[row_idx];
        if (row.source_line <= 0 || row.source_file.empty()) {
            continue;
        }

        size_t mapping_idx = index_by_key[std::make_pair(row.source_file, row.source_line)];
        mapped.instruction_to_mapping[row_idx] = mapping_idx;

        source_line_mapping& mapping = mapped.line_mappings[mapping_idx];
        mapping.total_instructions++;
        if (!mapping.spans.empty() && mapping.spans.back().first_row + mapping.spans.back().row_count == row_idx) {
            mapping.spans.back().row_count++;
        } else {
            mapping.spans.push_back({ row_idx, 1 });
        }
    }
}

bool source_paths_match(std::string_view recorded_path, std::string_view query_path) {
    if (recorded_path.find('\0') != std::string_view::npos || query_path.find('\0') != std::string_view::npos) {
        return false;
    }

    std::string recorded = normalize_lexical_path(recorded_path);
    std::string query = normalize_lexical_path(query_path);
    if (recorded.empty() || query.empty()) {
        return false;
    }

    if (recorded == query) {
        return true;
    }

    if (!query.starts_with('/') && recorded.size() > query.size() && recorded.ends_with(query) &&
        recorded[recorded.size() - query.size() - 1] == '/') {
        return true;
    }

    if (!recorded.starts_with('/') && query.size() > recorded.size() && query.ends_with(recorded) &&
        query[query.size() - recorded.size() - 1] == '/') {
        return true;
    }

    return false;
}

bool parse_objdump_disassembly(std::string_view objdump_output, disasm_map& mapped, std::string& error) {
    mapped = {};
    error.clear();

    std::string active_symbol;
    std::string active_source_file;
    int active_source_line = 0;

    std::string_view remaining = objdump_output;
    while (!remaining.empty()) {
        size_t newline = remaining.find('\n');
        std::string_view raw_line = remaining.substr(0, newline);
        remaining = newline == std::string_view::npos ? std::string_view{} : remaining.substr(newline + 1);
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.remove_suffix(1);
        }

        std::string_view trimmed = trim_ws(raw_line);
        if (trimmed.empty() || trimmed == "..." || trimmed.starts_with(DISASM_SECTION_PREFIX) ||
            is_file_format_banner(trimmed)) {
            continue;
        }

        uint64_t symbol_address = 0;
        std::string symbol_name;
        if (try_parse_symbol_header(raw_line, symbol_address, symbol_name)) {
            if (!mapped.symbols.empty()) {
                disasm_symbol_block& prev = mapped.symbols.back();
                prev.rows.row_count = mapped.instructions.size() - prev.rows.first_row;
            }

            active_symbol = symbol_name;
            active_source_file.clear();
            active_source_line = 0;
            mapped.symbols.push_back({ symbol_address, std::move(symbol_name), { mapped.instructions.size(), 0 } });
            continue;
        }

        uint64_t insn_addr = 0;
        std::vector<uint8_t> raw_bytes;
        std::string raw_bytes_text;
        std::string mnemonic_and_operands;
        if (try_parse_instruction_or_continuation(raw_line, insn_addr, raw_bytes, raw_bytes_text,
                                                  mnemonic_and_operands)) {
            if (mnemonic_and_operands.empty()) {
                if (!mapped.instructions.empty() && !raw_bytes.empty()) {
                    disasm_instruction_row& prev_row = mapped.instructions.back();
                    uint64_t prev_len = static_cast<uint64_t>(prev_row.raw_bytes.size());
                    if (prev_len <= UINT64_MAX - prev_row.address && insn_addr == prev_row.address + prev_len) {
                        prev_row.raw_bytes.insert(prev_row.raw_bytes.end(), raw_bytes.begin(), raw_bytes.end());
                        if (!prev_row.raw_bytes_text.empty()) {
                            prev_row.raw_bytes_text += ' ';
                        }

                        prev_row.raw_bytes_text += raw_bytes_text;
                    }
                }

                continue;
            }

            if (mapped.instructions.size() >= DISASM_MAX_INSTRUCTIONS) {
                mapped = {};
                error = "disassembly exceeds maximum supported instruction count";
                return false;
            }

            mapped.instructions.push_back({
                insn_addr, std::move(raw_bytes), std::move(raw_bytes_text),
                std::move(mnemonic_and_operands), active_symbol, active_source_file, active_source_line,
            });
            continue;
        }

        std::string marker_file;
        int marker_line = 0;
        bool clears_location = false;
        if (try_parse_source_marker(raw_line, marker_file, marker_line, clears_location)) {
            if (clears_location) {
                active_source_file.clear();
                active_source_line = 0;
            } else {
                active_source_file = std::move(marker_file);
                active_source_line = marker_line;
            }
        }
    }

    if (!mapped.symbols.empty()) {
        disasm_symbol_block& last = mapped.symbols.back();
        last.rows.row_count = mapped.instructions.size() - last.rows.first_row;
    }

    if (mapped.instructions.empty()) {
        error = "objdump output contains no disassembled instructions";
        return false;
    }

    build_line_mapping_tables(mapped);
    return true;
}

std::vector<instruction_row_span> spans_for_source_line(const disasm_map& mapped,
                                                        std::string_view source_file,
                                                        int source_line) {
    if (source_line <= 0) {
        return {};
    }

    std::string_view target = source_file.empty() ? std::string_view(mapped.primary_source_file) : source_file;
    if (target.empty()) {
        return {};
    }

    std::vector<instruction_row_span> collected;
    for (const source_line_mapping& entry : mapped.line_mappings) {
        if (entry.source_line == source_line && source_paths_match(entry.source_file, target)) {
            collected.insert(collected.end(), entry.spans.begin(), entry.spans.end());
        }
    }

    std::sort(collected.begin(), collected.end(),
              [](const instruction_row_span& lhs, const instruction_row_span& rhs) {
                  return lhs.first_row < rhs.first_row;
              });

    std::vector<instruction_row_span> merged;
    for (const instruction_row_span& span : collected) {
        if (!merged.empty() && merged.back().first_row + merged.back().row_count >= span.first_row) {
            size_t new_end = std::max(merged.back().first_row + merged.back().row_count,
                                      span.first_row + span.row_count);
            merged.back().row_count = new_end - merged.back().first_row;
        } else {
            merged.push_back(span);
        }
    }

    return merged;
}

bool source_for_instruction_row(const disasm_map& mapped, size_t row_index, std::string& source_file,
                                int& source_line, instruction_row_span& enclosing_span) {
    if (row_index >= mapped.instructions.size() || row_index >= mapped.instruction_to_mapping.size()) {
        return false;
    }

    size_t mapping_idx = mapped.instruction_to_mapping[row_index];
    if (mapping_idx >= mapped.line_mappings.size()) {
        return false;
    }

    const source_line_mapping& mapping = mapped.line_mappings[mapping_idx];
    for (const instruction_row_span& span : mapping.spans) {
        if (row_index >= span.first_row && row_index < span.first_row + span.row_count) {
            source_file = mapping.source_file;
            source_line = mapping.source_line;
            enclosing_span = span;
            return true;
        }
    }

    return false;
}

std::vector<std::pair<int, int>> build_split_view_line_pairs(const disasm_map& mapped,
                                                             std::string_view source_file) {
    std::string_view target = source_file.empty() ? std::string_view(mapped.primary_source_file) : source_file;
    if (target.empty()) {
        return {};
    }

    std::map<int, size_t> earliest_row_by_line;
    for (const source_line_mapping& entry : mapped.line_mappings) {
        if (entry.spans.empty() || !source_paths_match(entry.source_file, target)) {
            continue;
        }

        size_t first_row = entry.spans.front().first_row;
        if (first_row >= static_cast<size_t>(INT_MAX)) {
            continue;
        }

        auto [existing, inserted] = earliest_row_by_line.try_emplace(entry.source_line, first_row);
        if (!inserted && first_row < existing->second) {
            existing->second = first_row;
        }
    }

    std::vector<std::pair<int, int>> pairs;
    pairs.reserve(earliest_row_by_line.size());
    for (const auto& [src_line, row_idx] : earliest_row_by_line) {
        pairs.emplace_back(src_line, static_cast<int>(row_idx + 1));
    }

    return pairs;
}

disasm_gutter_profile compute_disasm_gutter_profile(const disasm_map& mapped,
                                                    std::span<const address_sample> samples,
                                                    std::string_view source_file,
                                                    uint64_t denominator_override) {
    disasm_gutter_profile profile;
    if (mapped.instructions.empty() || samples.empty()) {
        profile.denominator_weight = denominator_override;
        return profile;
    }

    std::vector<indexed_instruction_bounds> sorted_bounds;
    sorted_bounds.reserve(mapped.instructions.size());
    for (size_t row_idx = 0; row_idx < mapped.instructions.size(); row_idx++) {
        const disasm_instruction_row& row = mapped.instructions[row_idx];
        size_t byte_len = std::max<size_t>(row.raw_bytes.size(), 1);
        sorted_bounds.push_back({ row.address, saturating_add_u64(row.address, byte_len), row_idx });
    }

    std::sort(sorted_bounds.begin(), sorted_bounds.end(),
              [](const indexed_instruction_bounds& lhs, const indexed_instruction_bounds& rhs) {
                  return lhs.start != rhs.start ? (lhs.start < rhs.start) : (lhs.row_index < rhs.row_index);
              });

    for (size_t idx = 0; idx + 1 < sorted_bounds.size(); idx++) {
        uint64_t next_start = sorted_bounds[idx + 1].start;
        if (next_start > sorted_bounds[idx].start && next_start < sorted_bounds[idx].end) {
            sorted_bounds[idx].end = next_start;
        }
    }

    std::vector<uint64_t> row_weights(mapped.instructions.size(), 0);
    for (const address_sample& sample : samples) {
        if (sample.weight == 0) {
            continue;
        }

        auto upper = std::upper_bound(
            sorted_bounds.begin(), sorted_bounds.end(), sample.address,
            [](uint64_t target_addr, const indexed_instruction_bounds& candidate) {
                return target_addr < candidate.start;
            });
        if (upper == sorted_bounds.begin()) {
            continue;
        }

        const indexed_instruction_bounds& match = *(upper - 1);
        if (sample.address >= match.start && sample.address < match.end) {
            row_weights[match.row_index] = saturating_add_u64(row_weights[match.row_index], sample.weight);
            profile.total_matched_weight = saturating_add_u64(profile.total_matched_weight, sample.weight);
        }
    }

    profile.denominator_weight = denominator_override > 0 ? denominator_override : profile.total_matched_weight;
    double denom = static_cast<double>(profile.denominator_weight);

    for (size_t row_idx = 0; row_idx < row_weights.size(); row_idx++) {
        if (row_weights[row_idx] == 0) {
            continue;
        }

        double pct = profile.denominator_weight > 0 ? (PERCENT_SCALE * static_cast<double>(row_weights[row_idx]) / denom) : 0.0;
        profile.instruction_costs.push_back({ row_idx, row_weights[row_idx], pct });
    }

    std::string_view target = source_file.empty() ? std::string_view(mapped.primary_source_file) : source_file;
    if (target.empty()) {
        return profile;
    }

    std::map<int, std::pair<std::string, uint64_t>> weight_by_line;
    for (const source_line_mapping& mapping : mapped.line_mappings) {
        if (!source_paths_match(mapping.source_file, target)) {
            continue;
        }

        uint64_t line_weight = 0;
        for (const instruction_row_span& span : mapping.spans) {
            for (size_t offset = 0; offset < span.row_count; offset++) {
                line_weight = saturating_add_u64(line_weight, row_weights[span.first_row + offset]);
            }
        }

        if (line_weight > 0) {
            auto [entry, inserted] = weight_by_line.try_emplace(mapping.source_line,
                                                                std::make_pair(mapping.source_file, uint64_t{ 0 }));
            (void)inserted;
            entry->second.second = saturating_add_u64(entry->second.second, line_weight);
        }
    }

    profile.line_costs.reserve(weight_by_line.size());
    for (const auto& [src_line, file_and_weight] : weight_by_line) {
        double pct = profile.denominator_weight > 0
                         ? (PERCENT_SCALE * static_cast<double>(file_and_weight.second) / denom)
                         : 0.0;
        profile.line_costs.push_back({ file_and_weight.first, src_line, file_and_weight.second, pct });
    }

    return profile;
}

std::string format_gutter_percentage(double percent, uint64_t duration_ns) {
    char buffer[64] = {};
    if (duration_ns == 0) {
        std::snprintf(buffer, sizeof(buffer), "%.1f%%", percent);
        return buffer;
    }

    if (duration_ns >= NS_PER_MS) {
        double ms = static_cast<double>(duration_ns) / static_cast<double>(NS_PER_MS);
        std::snprintf(buffer, sizeof(buffer), "%.1f%%, %.2f ms", percent, ms);
        return buffer;
    }

    double us = static_cast<double>(duration_ns) / static_cast<double>(NS_PER_US);
    std::snprintf(buffer, sizeof(buffer), "%.1f%%, %.1f us", percent, us);
    return buffer;
}

} // namespace sheen
