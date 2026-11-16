#include "commands.hpp"

#include "boards.hpp"
#include "cores.hpp"

#include <algorithm>
#include <cctype>

struct board_entry {
    const installed_core*   core;
    const board_definition* definition;
    std::string             fqbn;
};

static std::string lowercased(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

static std::vector<board_entry> all_boards(const std::vector<installed_core>& cores) {
    std::vector<board_entry> entries;
    for (const installed_core& core : cores) {
        for (const board_definition& definition : core.boards) {
            std::string fqbn = format_fqbn(core.packager, core.architecture, definition.id, {});
            entries.push_back({ &core, &definition, fqbn });
        }
    }

    return entries;
}

// The board `word` names exactly, by its FQBN or by an ID no other installed board has, or nullptr
static const board_entry* find_exact(const std::vector<board_entry>& entries, const std::string& word) {
    const board_entry* by_id = nullptr;
    size_t id_matches = 0;
    for (const board_entry& entry : entries) {
        if (entry.fqbn == word) {
            return &entry;
        }

        if (entry.definition->id == word) {
            by_id = &entry;
            id_matches++;
        }
    }

    return id_matches == 1 ? by_id : nullptr;
}

static board_request request_for(const board_entry& entry) {
    return { entry.core->packager, entry.core->architecture, entry.definition->id, {} };
}

static int describe_board(const std::vector<installed_core>& cores, const board_request& request, reporter& report) {
    selected_board board;
    std::string error;
    if (!select_board(cores, request, board, error)) {
        report.failed({ failure_kind::board, error });
        return 1;
    }

    report.board_described(board);

    return 0;
}

// Boards whose FQBN or name holds `word`, ignoring case. A hidden board, and one mcufw cannot select, is left out, and
// naming it exactly shows it or the reason.
static int list_boards(const std::vector<installed_core>& cores, const std::vector<board_entry>& entries,
                       const std::string& word, reporter& report) {
    std::string lowered_word = lowercased(word);
    std::vector<listed_board> listed;
    for (const board_entry& entry : entries) {
        bool matches = lowercased(entry.fqbn).find(lowered_word) != std::string::npos ||
                       lowercased(entry.definition->name).find(lowered_word) != std::string::npos;
        if (entry.definition->hidden || !matches) {
            continue;
        }

        selected_board board;
        std::string error;
        if (!select_board(cores, request_for(entry), board, error)) {
            continue;
        }

        listed.push_back({ entry.fqbn, entry.definition->name });
    }

    std::sort(listed.begin(), listed.end(), [](const listed_board& a, const listed_board& b) {
        return a.fqbn < b.fqbn;
    });

    report.boards_listed(listed, word);

    return 0;
}

// A word holding an option names one board with those options, so it is described or its failure reported
int run_boards(const arguments& parsed, const directory_layout& layout, reporter& report) {
    std::vector<installed_core> cores;
    std::string error;
    if (!load_installed_cores(layout, cores, error)) {
        report.failed({ failure_kind::core, error });
        return 1;
    }

    std::string word = parsed.operands.empty() ? std::string() : parsed.operands[0];
    if (word.find('=') != std::string::npos) {
        board_request request;
        if (!parse_board_request(word, request, error)) {
            report.failed({ failure_kind::board, error });
            return 1;
        }

        return describe_board(cores, request, report);
    }

    std::vector<board_entry> entries = all_boards(cores);
    const board_entry* exact = word.empty() ? nullptr : find_exact(entries, word);
    if (exact) {
        return describe_board(cores, request_for(*exact), report);
    }

    return list_boards(cores, entries, word, report);
}
