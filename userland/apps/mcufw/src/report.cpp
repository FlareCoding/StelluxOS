#include "report.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>

constexpr size_t LISTING_GAP = 3;
constexpr size_t MENU_GAP = 2;
constexpr size_t DESCRIPTION_WIDTH_MAX = 120;
constexpr const char* NOTHING_NEEDED = "-";
constexpr const char* NO_VERSION = "-";
constexpr uint64_t PROGRESS_INTERVAL_NS = 100'000'000;
constexpr double BYTES_PER_MB = 1e6;
constexpr const char* CLEAR_TO_END = "\x1b[K";

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

void reporter::failed(const failure& failure) {
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
