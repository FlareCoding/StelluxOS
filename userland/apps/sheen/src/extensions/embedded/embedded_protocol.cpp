#include "extensions/embedded/embedded_protocol.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace sheen {

static constexpr std::string_view MIDDLE_DOT_SEPARATOR = " \xC2\xB7 ";
static constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";
static constexpr uint64_t PERCENT_SCALE = 100;
static constexpr uint64_t MEMORY_DANGER_PERCENT = 90;
static constexpr uint64_t BYTES_PER_KILOBYTE = 1024;
static constexpr uint64_t BYTES_PER_MEGABYTE = 1024 * 1024;
static constexpr int MAX_NESTED_SKETCH_DEPTH = 3;

namespace {

struct nested_sketch_candidate {
    int priority = 0;
    int depth = 0;
    std::string path;
};

} // namespace

static std::string_view strip_line_ending(std::string_view text) {
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }

    if (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
    }

    return text;
}

static std::string_view trim_ascii(std::string_view text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }

    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

static bool parse_u64(std::string_view text, uint64_t& number) {
    if (text.empty()) {
        return false;
    }

    const char* begin = text.data();
    const char* end = begin + text.size();
    std::from_chars_result parsed = std::from_chars(begin, end, number);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

static bool parse_u32(std::string_view text, uint32_t& number) {
    uint64_t wide = 0;
    if (!parse_u64(text, wide) || wide > UINT32_MAX) {
        return false;
    }

    number = static_cast<uint32_t>(wide);
    return true;
}

static std::string clean_optional_field(std::string_view field) {
    return field == "-" ? std::string() : std::string(field);
}

static bool has_sketch_extension(std::string_view name) {
    return name.ends_with(".ino") || name.ends_with(".pde");
}

static bool is_searchable_source_file(std::string_view name) {
    return has_sketch_extension(name)
        || name.ends_with(".h")
        || name.ends_with(".hpp")
        || name.ends_with(".hh")
        || name.ends_with(".cpp")
        || name.ends_with(".c");
}

static bool read_file_bytes(const std::filesystem::path& path, std::string& content) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }

    content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (std::string_view(content).starts_with(UTF8_BOM)) {
        content.erase(0, UTF8_BOM.size());
    }

    return true;
}

static bool is_regular_file_path(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

static bool is_directory_path(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

static bool is_symlink_entry(const std::filesystem::directory_entry& entry) {
    std::error_code ec;
    return entry.is_symlink(ec) && !ec;
}

std::string base_fqbn(std::string_view fqbn) {
    fqbn = trim_ascii(fqbn);
    size_t colons = 0;
    for (size_t index = 0; index < fqbn.size(); index++) {
        if (fqbn[index] == ':' && ++colons == 3) {
            return std::string(fqbn.substr(0, index));
        }
    }

    return std::string(fqbn);
}

board_catalog::board_catalog() : m_stream(event_protocol::TSV_MCUFW) {
}

void board_catalog::begin_refresh() {
    m_pending.clear();
    m_stream = event_line_stream(event_protocol::TSV_MCUFW);
    m_saw_failure = false;
}

void board_catalog::feed_stdout(std::string_view chunk) {
    m_stream.feed(chunk, [this](const std::vector<std::string>& fields) { handle_fields(fields); });
}

bool board_catalog::finish_refresh(int exit_code, bool was_stopped) {
    m_stream.flush([this](const std::vector<std::string>& fields) { handle_fields(fields); });
    if (exit_code != 0 || was_stopped || m_saw_failure) {
        m_pending.clear();
        m_saw_failure = false;
        return false;
    }

    m_boards = std::move(m_pending);
    m_pending.clear();
    return true;
}

const board_entry* board_catalog::find_by_fqbn(std::string_view fqbn) const {
    std::string wanted = base_fqbn(fqbn);
    for (const board_entry& entry : m_boards) {
        if (!wanted.empty() && entry.fqbn == wanted) {
            return &entry;
        }
    }

    return nullptr;
}

void board_catalog::handle_fields(const std::vector<std::string>& fields) {
    mcufw_event event = parse_mcufw_event_fields(fields);
    if (event.kind == mcufw_event_kind::BOARD) {
        m_pending.push_back({ std::move(event.fqbn), std::move(event.name), std::move(event.needs) });
    } else if (event.kind == mcufw_event_kind::FAILED) {
        m_saw_failure = true;
    }
}

static mcufw_event parse_mcufw_board_event(const std::vector<std::string>& fields) {
    mcufw_event event;
    if (fields.size() == 4 && !fields[1].empty()) {
        event.kind = mcufw_event_kind::BOARD;
        event.fqbn = fields[1];
        event.needs = clean_optional_field(fields[2]);
        event.name = fields[3];
    } else if (fields.size() == 3 && !fields[1].empty()) {
        event.kind = mcufw_event_kind::BOARD;
        event.fqbn = fields[1];
        event.name = fields[2];
    }

    return event;
}

static mcufw_event parse_mcufw_problem_event(const std::vector<std::string>& fields) {
    mcufw_event event;
    if (fields.size() < 6 || fields[2].empty()) {
        return event;
    }

    problem_level level = problem_level::ERROR;
    if (fields[1] == "error") {
        level = problem_level::ERROR;
    } else if (fields[1] == "warning") {
        level = problem_level::WARNING;
    } else if (fields[1] == "note") {
        level = problem_level::NOTE;
    } else {
        return event;
    }

    uint32_t line = 0;
    uint32_t column = 0;
    if (!parse_u32(fields[3], line) || line == 0 || !parse_u32(fields[4], column)) {
        return event;
    }

    event.kind = mcufw_event_kind::PROBLEM;
    event.severity = level;
    event.file_path = fields[2];

    event.line = static_cast<int>(line);
    event.column = static_cast<int>(column);
    event.message = fields[5];
    return event;
}

static mcufw_event parse_mcufw_size_event(const std::vector<std::string>& fields) {
    mcufw_event event;
    if (fields.size() < 4 || (fields[1] != "flash" && fields[1] != "ram")) {
        return event;
    }

    uint64_t used = 0;
    if (!parse_u64(fields[2], used)) {
        return event;
    }

    uint64_t maximum = 0;
    bool has_maximum = false;
    if (fields[3] != "-") {
        if (!parse_u64(fields[3], maximum)) {
            return event;
        }

        has_maximum = maximum > 0;
    }

    event.kind = mcufw_event_kind::SIZE;
    event.bank = fields[1] == "ram" ? memory_bank::RAM : memory_bank::FLASH;
    event.used_bytes = used;
    event.max_bytes = maximum;
    event.has_max_bytes = has_maximum;
    return event;
}

mcufw_event parse_mcufw_event_fields(const std::vector<std::string>& fields) {
    mcufw_event event;
    if (fields.empty()) {
        return event;
    }

    const std::string& tag = fields[0];
    if (tag == "board") {
        return parse_mcufw_board_event(fields);
    }

    if (tag == "problem") {
        return parse_mcufw_problem_event(fields);
    }

    if (tag == "size") {
        return parse_mcufw_size_event(fields);
    }

    if (tag == "library" && fields.size() >= 4 && !fields[1].empty()) {
        event.kind = mcufw_event_kind::LIBRARY;
        event.name = fields[1];
        event.version = fields[2];
        event.folder = fields[3];
    } else if (tag == "hint" && fields.size() >= 4 && !fields[1].empty() && !fields[2].empty()) {
        event.kind = mcufw_event_kind::HINT;
        event.header = fields[1];
        event.name = fields[2];
        event.version = fields[3];
    } else if (tag == "progress" && fields.size() >= 4 && !fields[1].empty()) {
        uint64_t done = 0;
        uint64_t total = 0;
        if (parse_u64(fields[2], done) && parse_u64(fields[3], total)) {
            event.kind = mcufw_event_kind::PROGRESS;
            event.stage = fields[1];
            event.done = done;
            event.total = total;
        }
    } else if (tag == "log" && fields.size() >= 3 && !fields[1].empty()) {
        event.kind = mcufw_event_kind::LOG;
        event.stage = fields[1];
        event.message = fields[2];
    } else if (tag == "start" && fields.size() >= 4 && !fields[1].empty()) {
        uint64_t total = 0;
        if (parse_u64(fields[3], total)) {
            event.kind = mcufw_event_kind::START;
            event.name = fields[1];
            event.version = fields[2];
            event.total = total;
        }
    } else if (tag == "installed" && fields.size() >= 3 && !fields[1].empty()) {
        event.kind = mcufw_event_kind::INSTALLED;
        event.name = fields[1];
        event.version = fields[2];
    } else if (tag == "needs" && fields.size() >= 3 && !fields[1].empty()) {
        event.kind = mcufw_event_kind::NEEDS;
        event.name = fields[1];
        event.version = clean_optional_field(fields[2]);
    } else if (tag == "warning" && fields.size() >= 2) {
        event.kind = mcufw_event_kind::WARNING;
        event.reason = fields[1];
    } else if (tag == "built" && fields.size() >= 2 && !fields[1].empty()) {
        event.kind = mcufw_event_kind::BUILT;
        event.file_path = fields[1];
    } else if (tag == "failed" && fields.size() >= 3 && !fields[1].empty()) {
        event.kind = mcufw_event_kind::FAILED;
        event.failure_kind = fields[1];
        event.reason = fields[2];
    }

    return event;
}

mcufw_event parse_mcufw_event_line(std::string_view line) {
    line = strip_line_ending(line);
    if (line.empty()) {
        return {};
    }

    return parse_mcufw_event_fields(split_event_line(event_protocol::TSV_MCUFW, line));
}

static std::string format_byte_quantity(uint64_t bytes) {
    char formatted[32];
    if (bytes >= BYTES_PER_MEGABYTE) {
        std::snprintf(formatted, sizeof(formatted), "%.1f MB",
                      static_cast<double>(bytes) / static_cast<double>(BYTES_PER_MEGABYTE));
        return formatted;
    }

    if (bytes >= BYTES_PER_KILOBYTE) {
        std::snprintf(formatted, sizeof(formatted), "%.1f KB",
                      static_cast<double>(bytes) / static_cast<double>(BYTES_PER_KILOBYTE));
        return formatted;
    }

    return std::to_string(bytes) + " B";
}

static std::string format_single_bank(std::string_view name, const memory_bank_usage& bank) {
    if (!bank.has_value) {
        return {};
    }

    if (bank.has_max && bank.max_bytes > 0) {
        uint64_t percent = bank.used_bytes * PERCENT_SCALE / bank.max_bytes;
        return std::string(name) + " " + std::to_string(percent) + "%";
    }

    return std::string(name) + " " + format_byte_quantity(bank.used_bytes);
}

void record_memory_size(memory_usage& usage, const mcufw_event& size_event) {
    if (size_event.kind != mcufw_event_kind::SIZE) {
        return;
    }

    memory_bank_usage& target = size_event.bank == memory_bank::RAM ? usage.ram : usage.flash;
    target.used_bytes = size_event.used_bytes;
    target.max_bytes = size_event.max_bytes;
    target.has_value = true;
    target.has_max = size_event.has_max_bytes && size_event.max_bytes > 0;
}

std::string format_memory_summary(const memory_usage& usage) {
    std::string flash_text = format_single_bank("Flash", usage.flash);
    std::string ram_text = format_single_bank("RAM", usage.ram);
    if (!flash_text.empty() && !ram_text.empty()) {
        return flash_text + std::string(MIDDLE_DOT_SEPARATOR) + ram_text;
    }

    return !flash_text.empty() ? flash_text : ram_text;
}

static bool is_bank_danger(const memory_bank_usage& bank) {
    if (!bank.has_value || !bank.has_max || bank.max_bytes == 0) {
        return false;
    }

    return bank.used_bytes > bank.max_bytes || bank.used_bytes * PERCENT_SCALE / bank.max_bytes >= MEMORY_DANGER_PERCENT;
}

bool is_memory_usage_danger(const memory_usage& usage) {
    return is_bank_danger(usage.flash) || is_bank_danger(usage.ram);
}

int compute_progress_percent(uint64_t done, uint64_t total) {
    if (total == 0) {
        return -1;
    }

    return done >= total ? static_cast<int>(PERCENT_SCALE) : static_cast<int>(done * PERCENT_SCALE / total);
}

std::string format_progress_label(std::string_view stage_or_item, uint64_t done, uint64_t total, bool is_install) {
    int percent = compute_progress_percent(done, total);
    if (is_install) {
        return percent >= 0
            ? "Installing " + std::string(stage_or_item) + " " + std::to_string(percent) + "%"
            : "Installing " + std::string(stage_or_item);
    }

    if (stage_or_item == "compile") {
        return total > 0 ? "Building " + std::to_string(done) + "/" + std::to_string(total) : std::string("Building");
    }

    if (stage_or_item == "libraries") {
        return "Resolving libraries";
    }

    return total > 0
        ? std::string(stage_or_item) + " " + std::to_string(done) + "/" + std::to_string(total)
        : std::string(stage_or_item);
}

bool append_problem_diagnostic(std::vector<embedded_diagnostic>& diagnostics, const mcufw_event& problem_event) {
    if (problem_event.kind != mcufw_event_kind::PROBLEM || problem_event.severity == problem_level::NOTE) {
        return false;
    }

    embedded_diagnostic item;
    item.file_path = problem_event.file_path;
    item.line = std::max(1, problem_event.line);
    item.column = std::max(0, problem_event.column);

    item.is_warning = problem_event.severity == problem_level::WARNING;
    item.message = problem_event.message;
    diagnostics.push_back(std::move(item));
    return true;
}

static bool line_includes_header(std::string_view line, std::string_view header) {
    line = trim_ascii(line);
    if (!line.starts_with('#')) {
        return false;
    }

    line = trim_ascii(line.substr(1));
    if (!line.starts_with("include")) {
        return false;
    }

    line = trim_ascii(line.substr(7));
    if (line.size() < header.size() + 2) {
        return false;
    }

    char open = line[0];
    char close = open == '<' ? '>' : (open == '"' ? '"' : '\0');
    size_t end = close == '\0' ? std::string_view::npos : line.find(close, 1);
    return end != std::string_view::npos && line.substr(1, end - 1) == header;
}

static bool find_header_include_in_file(const std::filesystem::path& file_path, std::string_view header,
                                        int& matched_line) {
    std::string text;
    if (!read_file_bytes(file_path, text)) {
        return false;
    }

    int line_number = 1;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        size_t stop = end == std::string::npos ? text.size() : end;
        if (line_includes_header(std::string_view(text.data() + start, stop - start), header)) {
            matched_line = line_number;
            return true;
        }

        if (end == std::string::npos) {
            break;
        }

        line_number++;
        start = end + 1;
    }

    return false;
}

static bool locate_missing_header_in_sketch(const std::string& sketch_dir, std::string_view header,
                                            std::string& file_path, int& line_number) {
    if (sketch_dir.empty() || header.empty()) {
        return false;
    }

    std::error_code iter_ec;
    std::vector<std::filesystem::path> candidates;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(sketch_dir, iter_ec)) {
        std::string name = entry.path().filename().string();
        if (is_regular_file_path(entry.path()) && !name.starts_with('.') && is_searchable_source_file(name)) {
            candidates.push_back(entry.path());
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        bool a_ino = has_sketch_extension(a.filename().string());
        bool b_ino = has_sketch_extension(b.filename().string());
        return a_ino != b_ino ? a_ino : a.string() < b.string();
    });

    for (const std::filesystem::path& candidate : candidates) {
        int matched = 1;
        if (find_header_include_in_file(candidate, header, matched)) {
            file_path = candidate.string();
            line_number = matched;
            return true;
        }
    }

    return false;
}

static void apply_hint_fields(embedded_diagnostic& target, const mcufw_event& hint_event) {
    target.fix_library = hint_event.name;
    target.fix_version = hint_event.version;
    target.fix_header = hint_event.header;

    std::string suffix = "Provided by " + hint_event.name;
    if (!hint_event.version.empty()) {
        suffix += " " + hint_event.version;
    }

    if (target.message.find(suffix) != std::string::npos) {
        return;
    }

    if (!target.message.empty()) {
        target.message += target.message.back() == '.' ? " " : ". ";
    }

    target.message += suffix;
}

bool attach_library_hint(std::vector<embedded_diagnostic>& diagnostics, const mcufw_event& hint_event,
                         const std::string& sketch_dir) {
    if (hint_event.kind != mcufw_event_kind::HINT || hint_event.name.empty()) {
        return false;
    }

    for (size_t index = diagnostics.size(); index > 0; index--) {
        embedded_diagnostic& item = diagnostics[index - 1];
        if (!item.is_warning && !hint_event.header.empty() && item.message.find(hint_event.header) != std::string::npos) {
            apply_hint_fields(item, hint_event);
            return true;
        }
    }

    for (size_t index = diagnostics.size(); index > 0; index--) {
        embedded_diagnostic& item = diagnostics[index - 1];
        if (!item.is_warning && item.fix_library.empty()) {
            apply_hint_fields(item, hint_event);
            return true;
        }
    }

    std::string include_file;
    int include_line = 1;
    if (!locate_missing_header_in_sketch(sketch_dir, hint_event.header, include_file, include_line)) {
        return false;
    }

    embedded_diagnostic synthesized;
    synthesized.file_path = std::move(include_file);
    synthesized.line = include_line;
    synthesized.message = hint_event.header + ": No such file or directory";

    apply_hint_fields(synthesized, hint_event);
    diagnostics.push_back(std::move(synthesized));
    return true;
}

static bool parse_yaml_top_key(std::string_view line, std::string_view key, std::string& out_value) {
    if (!line.starts_with(key) || line.size() <= key.size() || line[key.size()] != ':') {
        return false;
    }

    std::string_view rest = line.substr(key.size() + 1);
    size_t first = rest.find_first_not_of(" \t");
    rest = first == std::string_view::npos ? std::string_view() : rest.substr(first);
    if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
        size_t close = rest.find(rest.front(), 1);
        out_value = std::string(rest.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
        return true;
    }

    rest = rest.substr(0, rest.find(" #"));
    size_t last = rest.find_last_not_of(" \t\r");
    out_value = last == std::string_view::npos ? std::string() : std::string(rest.substr(0, last + 1));
    return true;
}

sketch_defaults parse_sketch_yaml(std::string_view text) {
    sketch_defaults defaults;
    if (text.starts_with(UTF8_BOM)) {
        text.remove_prefix(UTF8_BOM.size());
    }

    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        size_t length = end == std::string_view::npos ? text.size() - start : end - start;
        std::string_view line = strip_line_ending(text.substr(start, length));
        parse_yaml_top_key(line, "default_fqbn", defaults.fqbn);

        if (end == std::string_view::npos) {
            break;
        }

        start = end + 1;
    }

    return defaults;
}

sketch_defaults read_sketch_defaults(const std::string& sketch_dir) {
    if (sketch_dir.empty()) {
        return {};
    }

    std::filesystem::path root(sketch_dir);
    std::filesystem::path yaml_path = root / "sketch.yaml";
    if (!is_regular_file_path(yaml_path)) {
        yaml_path = root / "sketch.yml";
        if (!is_regular_file_path(yaml_path)) {
            return {};
        }
    }

    std::string content;
    return read_file_bytes(yaml_path, content) ? parse_sketch_yaml(content) : sketch_defaults{};
}

bool is_path_within_workspace(std::string_view candidate_path, std::string_view workspace_root) {
    while (workspace_root.size() > 1 && workspace_root.back() == '/') {
        workspace_root.remove_suffix(1);
    }

    while (candidate_path.size() > 1 && candidate_path.back() == '/') {
        candidate_path.remove_suffix(1);
    }

    if (candidate_path.empty() || workspace_root.empty()) {
        return false;
    }

    if (workspace_root == "/") {
        return candidate_path.starts_with('/');
    }

    return candidate_path == workspace_root
        || (candidate_path.starts_with(workspace_root)
            && candidate_path.size() > workspace_root.size()
            && candidate_path[workspace_root.size()] == '/');
}

static bool directory_has_strict_sketch_marker(const std::filesystem::path& dir) {
    if (!is_directory_path(dir)) {
        return false;
    }

    if (is_regular_file_path(dir / "sketch.yaml") || is_regular_file_path(dir / "sketch.yml")) {
        return true;
    }

    std::string folder_name = dir.filename().string();
    return !folder_name.empty()
        && (is_regular_file_path(dir / (folder_name + ".ino"))
            || is_regular_file_path(dir / (folder_name + ".pde")));
}

static bool directory_has_any_sketch_file(const std::filesystem::path& dir) {
    if (directory_has_strict_sketch_marker(dir)) {
        return true;
    }

    std::error_code iter_ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, iter_ec)) {
        std::string name = entry.path().filename().string();
        if (is_regular_file_path(entry.path()) && !name.starts_with('.') && has_sketch_extension(name)) {
            return true;
        }
    }

    return false;
}

static void search_nested_sketches(const std::filesystem::path& dir, int depth,
                                   std::vector<nested_sketch_candidate>& matches) {
    if (depth > MAX_NESTED_SKETCH_DEPTH) {
        return;
    }

    std::error_code iter_ec;
    std::vector<std::filesystem::path> subdirs;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, iter_ec)) {
        if (!is_directory_path(entry.path()) || is_symlink_entry(entry)) {
            continue;
        }

        std::string name = entry.path().filename().string();
        bool is_build_dir = name == "build" || is_regular_file_path(entry.path() / "build.options.json");
        if (!name.starts_with('.') && !is_build_dir) {
            subdirs.push_back(entry.path());
        }
    }

    std::sort(subdirs.begin(), subdirs.end());
    for (const std::filesystem::path& child : subdirs) {
        if (directory_has_strict_sketch_marker(child)) {
            matches.push_back({ 0, depth, child.string() });
            continue;
        }

        if (directory_has_any_sketch_file(child)) {
            matches.push_back({ 1, depth, child.string() });
        }

        search_nested_sketches(child, depth + 1, matches);
    }
}

std::string resolve_sketch_directory(const std::string& workspace_or_file_path) {
    if (workspace_or_file_path.empty()) {
        return {};
    }

    std::error_code canon_ec;
    std::filesystem::path start_path = std::filesystem::weakly_canonical(workspace_or_file_path, canon_ec);
    if (canon_ec || start_path.empty()) {
        start_path = std::filesystem::path(workspace_or_file_path).lexically_normal();
    }

    if (is_regular_file_path(start_path) || has_sketch_extension(start_path.filename().string())) {
        start_path = start_path.parent_path();
    }

    if (directory_has_strict_sketch_marker(start_path)) {
        return start_path.string();
    }

    for (std::filesystem::path ancestor = start_path.parent_path();
         !ancestor.empty() && ancestor != ancestor.parent_path();
         ancestor = ancestor.parent_path()) {
        if (directory_has_strict_sketch_marker(ancestor)) {
            return ancestor.string();
        }
    }

    if (directory_has_any_sketch_file(start_path)) {
        return start_path.string();
    }

    std::vector<nested_sketch_candidate> nested;
    search_nested_sketches(start_path, 1, nested);
    if (!nested.empty()) {
        std::sort(nested.begin(), nested.end(),
                  [](const nested_sketch_candidate& a, const nested_sketch_candidate& b) {
                      if (a.priority != b.priority) {
                          return a.priority < b.priority;
                      }

                      return a.depth != b.depth ? a.depth < b.depth : a.path < b.path;
                  });
        return nested.front().path;
    }

    return start_path.string();
}

static bool parse_library_properties_file(const std::filesystem::path& prop_path, std::string& name,
                                          std::string& version) {
    std::string text;
    if (!read_file_bytes(prop_path, text)) {
        return false;
    }

    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        size_t length = end == std::string_view::npos ? text.size() - start : end - start;
        std::string_view line = trim_ascii(std::string_view(text.data() + start, length));
        if (!line.empty() && !line.starts_with('#')) {
            size_t eq = line.find('=');
            if (eq != std::string_view::npos) {
                std::string_view key = trim_ascii(line.substr(0, eq));
                std::string_view property_value = trim_ascii(line.substr(eq + 1));
                if (key == "name") {
                    name = std::string(property_value);
                } else if (key == "version") {
                    version = std::string(property_value);
                }
            }
        }

        if (end == std::string::npos) {
            break;
        }

        start = end + 1;
    }

    return true;
}

static bool directory_holds_header(const std::filesystem::path& dir) {
    std::error_code iter_ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, iter_ec)) {
        std::string name = entry.path().filename().string();
        if (is_regular_file_path(entry.path())
            && (name.ends_with(".h") || name.ends_with(".hpp") || name.ends_with(".hh"))) {
            return true;
        }
    }

    return false;
}

static void collect_installed_cores(const std::string& hardware_dir, std::vector<installed_library>& items) {
    if (hardware_dir.empty() || !is_directory_path(hardware_dir)) {
        return;
    }

    std::error_code pkg_ec;
    for (const std::filesystem::directory_entry& packager : std::filesystem::directory_iterator(hardware_dir, pkg_ec)) {
        if (!is_directory_path(packager.path()) || packager.path().filename().string().starts_with('.')) {
            continue;
        }

        std::error_code arch_ec;
        for (const std::filesystem::directory_entry& arch : std::filesystem::directory_iterator(packager.path(), arch_ec)) {
            if (!is_directory_path(arch.path()) || arch.path().filename().string().starts_with('.')) {
                continue;
            }

            std::string arch_name = arch.path().filename().string();
            std::error_code ver_ec;
            for (const std::filesystem::directory_entry& ver : std::filesystem::directory_iterator(arch.path(), ver_ec)) {
                if (is_directory_path(ver.path()) && !ver.path().filename().string().starts_with('.')) {
                    items.push_back({ arch_name + "-core", ver.path().filename().string(), ver.path().string(), true });
                }
            }
        }
    }
}

std::vector<installed_library> scan_installed_libraries(const std::string& libraries_dir,
                                                        const std::string& hardware_dir) {
    std::vector<installed_library> items;
    if (!libraries_dir.empty() && is_directory_path(libraries_dir)) {
        std::error_code iter_ec;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(libraries_dir, iter_ec)) {
            std::string folder = entry.path().filename().string();
            if (!is_directory_path(entry.path()) || folder.starts_with('.')) {
                continue;
            }

            std::filesystem::path prop_path = entry.path() / "library.properties";
            if (is_regular_file_path(prop_path)) {
                std::string name;
                std::string version;
                if (parse_library_properties_file(prop_path, name, version)) {
                    items.push_back({ name.empty() ? folder : name, version, entry.path().string(), false });
                }
            } else if (directory_holds_header(entry.path())) {
                items.push_back({ folder, {}, entry.path().string(), false });
            }
        }
    }

    collect_installed_cores(hardware_dir, items);
    std::sort(items.begin(), items.end(), [](const installed_library& a, const installed_library& b) {
        if (a.is_core != b.is_core) {
            return !a.is_core;
        }

        return a.name != b.name ? a.name < b.name : a.version < b.version;
    });

    return items;
}

} // namespace sheen
