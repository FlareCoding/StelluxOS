#include "report.hpp"

#include "build.hpp"
#include "upload.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>

constexpr size_t LISTING_GAP = 3;
constexpr size_t MENU_GAP = 2;
constexpr size_t DESCRIPTION_WIDTH_MAX = 120;
constexpr const char* NOTHING_NEEDED = "-";
constexpr const char* NO_VERSION = "-";
constexpr const char* NO_MAXIMUM = "-";
constexpr uint64_t PROGRESS_INTERVAL_NS = 100'000'000;
constexpr double BYTES_PER_MB = 1e6;
constexpr const char* CLEAR_TO_END = "\x1b[K";

constexpr size_t LABEL_WIDTH = 11;
constexpr uint64_t NANOSECONDS_PER_SECOND = 1'000'000'000;
constexpr uint64_t SECONDS_PER_MINUTE = 60;
constexpr uint64_t PERCENT = 100;

struct stage_label {
    const char* stage;
    const char* label;
    bool        summarized;  // its final count is cleared, since `libraries_found` names the stage's result
};

constexpr stage_label STAGE_LABELS[] = {
    { "libraries", "Libraries", true },
    { "compile", "Compiling", false },
};

reporter::reporter(bool events, FILE* out, FILE* err)
    : m_events(events), m_terminal(isatty(fileno(out)) != 0), m_out(out), m_err(err) {}

static const char* failure_kind_name(failure_kind kind) {
    switch (kind) {
    case failure_kind::board:
        return "board";
    case failure_kind::core:
        return "core";
    case failure_kind::library:
        return "library";
    case failure_kind::compile:
        return "compile";
    case failure_kind::recipe:
        return "recipe";
    case failure_kind::link:
        return "link";
    case failure_kind::size:
        return "size";
    case failure_kind::port:
        return "port";
    case failure_kind::flash:
        return "flash";
    case failure_kind::network:
        return "network";
    case failure_kind::verify:
        return "verify";
    case failure_kind::lock:
        return "lock";
    case failure_kind::cancelled:
        return "cancelled";
    case failure_kind::io:
        return "io";
    }

    return "io";
}

void reporter::event(const std::vector<std::string>& fields) {
    std::string line;
    for (const std::string& field : fields) {
        if (!line.empty()) {
            line += '\t';
        }

        std::string written = field;
        std::replace(written.begin(), written.end(), '\t', ' ');
        std::replace(written.begin(), written.end(), '\n', ' ');
        line += written;
    }

    fprintf(m_out, "%s\n", line.c_str());
}

static std::string needs_field(const board_requirements& requirements) {
    std::string missing = describe_missing(requirements);
    return missing.empty() ? NOTHING_NEEDED : missing;
}

static std::string padded(const std::string& text, size_t width) {
    return text + std::string(width > text.size() ? width - text.size() : 0, ' ');
}

void reporter::boards_listed(const std::vector<listed_board>& boards, const std::string& word) {
    if (m_events) {
        for (const listed_board& board : boards) {
            event({ "board", board.fqbn, needs_field(board.requirements), board.name });
        }

        return;
    }

    if (boards.empty()) {
        if (word.empty()) {
            fprintf(m_out, "No installed core defines a board. Install a core with mcufw install.\n");
        } else {
            fprintf(m_out, "No installed board matches %s.\n", word.c_str());
        }

        return;
    }

    size_t fqbn_width = 0;
    size_t name_width = 0;
    for (const listed_board& board : boards) {
        fqbn_width = std::max(fqbn_width, board.fqbn.size());
        name_width = std::max(name_width, board.name.size());
    }

    for (const listed_board& board : boards) {
        std::string missing = describe_missing(board.requirements);
        std::string line = padded(board.fqbn, fqbn_width + LISTING_GAP);
        line += missing.empty() ? board.name : padded(board.name, name_width + LISTING_GAP) + "needs " + missing;
        fprintf(m_out, "%s\n", line.c_str());
    }
}

static size_t description_width(FILE* out) {
    winsize size{};
    if (ioctl(fileno(out), TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
        return DESCRIPTION_WIDTH_MAX;
    }

    return std::min<size_t>(size.ws_col, DESCRIPTION_WIDTH_MAX);
}

// A menu's choices after its ID, each continuation line starting under the first choice
static std::string menu_row(const board_menu& menu, size_t indent, size_t width) {
    std::string row = padded(menu.id, indent);
    size_t line_start = 0;
    for (const menu_choice& choice : menu.choices) {
        bool is_first_on_line = row.size() - line_start == indent;
        if (!is_first_on_line && row.size() - line_start + 1 + choice.id.size() > width) {
            row += "\n";
            line_start = row.size();
            row += std::string(indent, ' ');
            is_first_on_line = true;
        }

        row += is_first_on_line ? choice.id : " " + choice.id;
    }

    return row;
}

void reporter::board_described(const selected_board& board, const board_requirements& requirements) {
    const board_definition& definition = *board.definition;
    if (m_events) {
        event({ "board", full_fqbn(board), needs_field(requirements), definition.name });
        for (const board_menu& menu : definition.menus) {
            for (const menu_choice& choice : menu.choices) {
                event({ "option", menu.id, choice.id, choice.label });
            }
        }

        return;
    }

    const installed_core& core = *board.core;
    std::string platform_name = core.platform.get("name");
    if (platform_name.empty()) {
        platform_name = core.packager + ":" + core.architecture;
    }

    std::string header = requested_fqbn(board) + std::string(LISTING_GAP, ' ') + definition.name + ", from " +
                         platform_name + " " + core.version;
    fprintf(m_out, "%s\n", header.c_str());

    std::string missing = describe_missing(requirements);
    if (!missing.empty()) {
        fprintf(m_out, "needs %s\n", missing.c_str());
    }

    if (definition.menus.empty()) {
        return;
    }

    size_t indent = 0;
    for (const board_menu& menu : definition.menus) {
        indent = std::max(indent, menu.id.size() + MENU_GAP);
    }

    fprintf(m_out, "\n");
    size_t width = description_width(m_out);
    for (const board_menu& menu : definition.menus) {
        fprintf(m_out, "%s\n", menu_row(menu, indent, width).c_str());
    }
}

static std::string megabytes(uint64_t bytes) {
    char text[32];
    snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / BYTES_PER_MB);
    return text;
}

void reporter::install_started(const std::string& item, const std::string& version, uint64_t length) {
    m_last_progress_ns = 0;
    if (m_events) {
        event({ "start", item, version, std::to_string(length) });
        return;
    }

    m_progress_line = "Installing " + item + " " + version;
    fprintf(m_out, "%s, %s%s", m_progress_line.c_str(), megabytes(length).c_str(), m_terminal ? "" : "\n");
    fflush(m_out);
}

void reporter::install_progress(const std::string& item, uint64_t done, uint64_t length, uint64_t now_ns) {
    bool is_due = done == length || now_ns - m_last_progress_ns >= PROGRESS_INTERVAL_NS;
    if (!is_due) {
        return;
    }

    m_last_progress_ns = now_ns;
    if (m_events) {
        event({ "progress", item, std::to_string(done), std::to_string(length) });
        return;
    }

    if (m_terminal) {
        fprintf(m_out, "\r%s, %s of %s%s", m_progress_line.c_str(), megabytes(done).c_str(),
                megabytes(length).c_str(), CLEAR_TO_END);
        fflush(m_out);
    }
}

void reporter::installed(const std::string& item, const std::string& version, bool already_installed) {
    if (m_events) {
        event({ "installed", item, version });
        return;
    }

    std::string line = already_installed ? item + " " + version + " is installed already"
                                         : "Installed " + item + " " + version;
    fprintf(m_out, "%s%s%s\n", m_terminal ? "\r" : "", line.c_str(), m_terminal ? CLEAR_TO_END : "");
    m_progress_line.clear();
}

void reporter::removed(const std::string& item, const std::string& version) {
    if (m_events) {
        event({ "removed", item, version });
        return;
    }

    fprintf(m_out, "Removed %s %s\n", item.c_str(), version.c_str());
}

void reporter::program_missing(const tool_dependency& tool) {
    std::string name = tool.packager.empty() ? tool.name : tool.packager + ":" + tool.name;
    if (m_events) {
        event({ "needs", name, tool.version.empty() ? NO_VERSION : tool.version });
        return;
    }

    std::string program = tool.version.empty() ? name : name + " " + tool.version;
    fprintf(m_out, "The board needs %s, a program only a Stellux package installs.\n", program.c_str());
}

void reporter::warned(const std::string& reason) {
    if (!m_events && m_terminal && !m_progress_line.empty()) {
        fprintf(m_out, "\n");
        m_progress_line.clear();
    }

    if (m_events) {
        event({ "warning", reason });
        return;
    }

    fprintf(m_err, "mcufw: %s\n", reason.c_str());
}

static std::string platform_name(const installed_core& core) {
    std::string name = core.platform.get("name");
    return name.empty() ? core.packager + ":" + core.architecture : name;
}

static std::string with_thousands(uint64_t number) {
    std::string digits = std::to_string(number);
    for (size_t at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }

    return digits;
}

static std::string duration(uint64_t elapsed_ns) {
    uint64_t seconds = (elapsed_ns + NANOSECONDS_PER_SECOND / 2) / NANOSECONDS_PER_SECOND;
    if (seconds < SECONDS_PER_MINUTE) {
        return std::to_string(seconds) + " s";
    }

    return std::to_string(seconds / SECONDS_PER_MINUTE) + " min " + std::to_string(seconds % SECONDS_PER_MINUTE) + " s";
}

void reporter::labeled(std::string_view label, const std::string& text) {
    hide_progress();
    fprintf(m_out, "%s%s\n", padded(std::string(label), LABEL_WIDTH).c_str(), text.c_str());
    fflush(m_out);
}

void reporter::hide_progress() {
    if (m_shown_count.empty()) {
        return;
    }

    fprintf(m_out, "\r%s", CLEAR_TO_END);
    fflush(m_out);
    m_shown_count.clear();
}

void reporter::sketch_found(const sketch& sketch) {
    if (m_events) {
        event({ "sketch", sketch.name, sketch.folder });
    }
}

void reporter::board_selected(const selected_board& board, bool saved) {
    if (m_events) {
        event({ "board", full_fqbn(board), board.definition->name });
        return;
    }

    std::string text = board.definition->name + " (" + requested_fqbn(board) + ")";
    labeled("Board", saved ? text + ", saved in sketch.yaml" : text);
}

void reporter::core_compiling(const installed_core& core, const std::string& archive, size_t files) {
    if (m_events) {
        event({ "core", "compile", archive });
        return;
    }

    std::string count = files == 1 ? "1 file" : std::to_string(files) + " files";
    labeled("Core", platform_name(core) + " " + core.version + ", " + count);
}

void reporter::core_cached(const std::string& archive) {
    if (m_events) {
        event({ "core", "cached", archive });
        return;
    }

    labeled("Core", "cached");
}

void reporter::libraries_found(const std::vector<const library*>& libraries) {
    if (m_events) {
        for (const library* found : libraries) {
            event({ "library", found->name, found->version, found->directory });
        }

        return;
    }

    std::string list;
    for (const library* found : libraries) {
        list += (list.empty() ? "" : ", ") + found->name + (found->version.empty() ? "" : " " + found->version);
    }

    if (!list.empty()) {
        labeled("Libraries", list);
    }
}

void reporter::library_suggested(const std::string& header, const library_suggestion& suggestion) {
    if (m_events) {
        event({ "hint", header, suggestion.name, suggestion.version });
        return;
    }

    bool has_space = suggestion.name.find(' ') != std::string::npos;
    std::string target = has_space ? "\"" + suggestion.name + "\"" : suggestion.name;
    hide_progress();
    fprintf(m_err, "%s %s provides %s: mcufw install %s\n", suggestion.name.c_str(), suggestion.version.c_str(),
            header.c_str(), target.c_str());
}

static std::string joined_with_commas(const std::vector<std::string>& words) {
    std::string joined;
    for (const std::string& word : words) {
        joined += (joined.empty() ? "" : ", ") + word;
    }

    return joined;
}

void reporter::library_incompatible(const library& library, const std::vector<std::string>& architectures) {
    tool_output("libraries", "WARNING: library " + library.name + " claims to run on " +
                                 joined_with_commas(library.architectures) +
                                 " architecture(s) and may be incompatible with your current board which runs on " +
                                 joined_with_commas(architectures) + " architecture(s).");
}

void reporter::progress(std::string_view stage, uint64_t done, uint64_t total, uint64_t now_ns) {
    bool finished = done >= total;
    if (!finished && now_ns - m_last_progress_ns < PROGRESS_INTERVAL_NS) {
        return;
    }

    m_last_progress_ns = finished ? 0 : now_ns;
    if (m_events) {
        event({ "progress", std::string(stage), std::to_string(done), std::to_string(total) });
        return;
    }

    std::string label(stage);
    bool summarized = false;
    for (const stage_label& known : STAGE_LABELS) {
        if (stage == known.stage) {
            label = known.label;
            summarized = known.summarized;
        }
    }

    std::string count = std::to_string(done) + " of " + std::to_string(total);
    if (finished && summarized) {
        hide_progress();
        return;
    }

    if (finished && stage == "compile" && m_compiles_total > 0) {
        std::string reason = m_compile_reason.empty() ? "" : ", " + m_compile_reason;
        labeled(label, std::to_string(m_compiles_needed) + " of " + std::to_string(m_compiles_total) + reason);
        return;
    }

    if (finished) {
        labeled(label, count);
        return;
    }

    if (m_terminal) {
        m_shown_count = padded(label, LABEL_WIDTH) + count;
        fprintf(m_out, "\r%s%s", m_shown_count.c_str(), CLEAR_TO_END);
        fflush(m_out);
    }
}

void reporter::command_started(const std::vector<std::string>& words) {
    if (m_events) {
        return;
    }

    std::string command;
    for (const std::string& word : words) {
        command += command.empty() ? "" : " ";
        if (word.find_first_of("\"\\ \t") == std::string::npos) {
            command += word;
            continue;
        }

        command += '"';
        for (char c : word) {
            command += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
        }

        command += '"';
    }

    std::string shown = m_shown_count;
    hide_progress();
    fprintf(m_out, "%s\n", command.c_str());
    if (!shown.empty()) {
        fprintf(m_out, "%s", shown.c_str());
        fflush(m_out);
        m_shown_count = shown;
    }
}

void reporter::tool_output(std::string_view stage, std::string_view text) {
    if (m_events) {
        while (!text.empty()) {
            size_t end = text.find('\n');
            std::string_view line = text.substr(0, end);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }

            event({ "log", std::string(stage), std::string(line) });
            text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        }

        return;
    }

    std::string shown = m_shown_count;
    hide_progress();
    fwrite(text.data(), 1, text.size(), m_err);
    if (!text.empty() && text.back() != '\n') {
        fputc('\n', m_err);
    }

    fflush(m_err);
    if (!shown.empty()) {
        fprintf(m_out, "%s", shown.c_str());
        fflush(m_out);
        m_shown_count = shown;
    }
}

static const char* severity_name(problem_severity severity) {
    switch (severity) {
    case problem_severity::error:
        return "error";
    case problem_severity::warning:
        return "warning";
    case problem_severity::note:
        return "note";
    }

    return "error";
}

void reporter::problem(const compiler_problem& problem) {
    if (m_events) {
        event({ "problem", severity_name(problem.severity), problem.file, std::to_string(problem.line),
                std::to_string(problem.column), problem.message });
    }
}

// `<used> of <maximum> bytes, <share>%`, or the bytes alone when the maximum is 0 or unset
static std::string usage(uint64_t used, uint64_t maximum) {
    if (maximum == 0) {
        return with_thousands(used) + " bytes";
    }

    return with_thousands(used) + " of " + with_thousands(maximum) + " bytes, " +
           std::to_string(used * PERCENT / maximum) + "%";
}

void reporter::size_checked(const program_size& size, uint64_t flash_max, uint64_t ram_max) {
    if (m_events) {
        event({ "size", "flash", std::to_string(size.flash), flash_max ? std::to_string(flash_max) : NO_MAXIMUM });
        if (size.has_ram) {
            event({ "size", "ram", std::to_string(size.ram), ram_max ? std::to_string(ram_max) : NO_MAXIMUM });
        }

        return;
    }

    labeled("Flash", usage(size.flash, flash_max));
    if (size.has_ram) {
        labeled("RAM", usage(size.ram, ram_max));
    }
}

void reporter::compiles_needed(size_t needed, size_t total, const std::string& first_reason) {
    m_compiles_needed = needed;
    m_compiles_total = total;
    m_compile_reason = first_reason;
}

void reporter::built(const build_result& result, uint64_t elapsed_ns) {
    if (m_events) {
        event({ "built", result.image });
        return;
    }

    labeled("Built", result.image + (result.up_to_date ? " is up to date" : " in " + duration(elapsed_ns)));
}

void reporter::port_chosen(const std::string& path, const std::string& why) {
    if (m_events) {
        event({ "port", path });
        return;
    }

    labeled("Port", path + ", " + why);
}

void reporter::end_upload_line() {
    if (m_upload_line.empty()) {
        return;
    }

    event({ "log", "flash", m_upload_line });
    int percent = last_percentage(m_upload_line);
    if (percent >= 0) {
        event({ "progress", "flash", std::to_string(percent), "100" });
    }

    m_upload_line.clear();
}

void reporter::upload_output(std::string_view bytes) {
    if (!m_events) {
        fwrite(bytes.data(), 1, bytes.size(), m_err);
        fflush(m_err);
        return;
    }

    for (char c : bytes) {
        if (c == '\r' || c == '\n') {
            end_upload_line();
        } else {
            m_upload_line += c;
        }
    }
}

void reporter::flashed(const std::string& port, uint64_t elapsed_ns) {
    if (m_events) {
        end_upload_line();
        event({ "flashed", port });
        return;
    }

    labeled("Flashed", "in " + duration(elapsed_ns));
}

void reporter::decoding(const std::string& elf) {
    fprintf(m_out, "%s%s\r\n", padded("Decoding", LABEL_WIDTH).c_str(), elf.c_str());
    fflush(m_out);
}

void reporter::decoding_off(const std::string& why) {
    fprintf(m_out, "%soff, %s\r\n", padded("Decoding", LABEL_WIDTH).c_str(), why.c_str());
    fflush(m_out);
}

void reporter::board_output(std::string_view bytes) {
    fwrite(bytes.data(), 1, bytes.size(), m_out);
    fflush(m_out);
}

void reporter::decoded_lines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        fprintf(m_out, "%s\r\n", line.c_str());
    }

    fflush(m_out);
}

void reporter::failed(const failure& failure) {
    hide_progress();
    if (!m_events && m_terminal && !m_progress_line.empty()) {
        fprintf(m_out, "\n");
        m_progress_line.clear();
    }

    if (m_events) {
        event({ "failed", failure_kind_name(failure.kind), failure.reason });
        return;
    }

    fprintf(m_err, "mcufw: %s\n", failure.reason.c_str());
}

uint64_t monotonic_clock_ns() {
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);

    return static_cast<uint64_t>(now.tv_sec) * NANOSECONDS_PER_SECOND + static_cast<uint64_t>(now.tv_nsec);
}
