#ifndef MCUFW_REPORT_HPP
#define MCUFW_REPORT_HPP

// What a command tells its caller: sentences on stdout and failures on stderr, or with `--events` one fact per line on
// stdout in tab-separated fields, a tab or newline in a field written as a space. main prints usage errors itself.

#include "boards.hpp"
#include "failure.hpp"
#include "requirements.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct listed_board {
    std::string        fqbn;
    std::string        name;
    board_requirements requirements;
};

class reporter {
public:
    reporter(bool events, FILE* out, FILE* err);

    // A person sees aligned columns, with `needs` and what after a board missing something, or a sentence when none
    // matched `word`. A program sees `board <fqbn> <needs> <name>`, `<needs>` from `describe_missing` or `-` if none.
    void boards_listed(const std::vector<listed_board>& boards, const std::string& word);

    // The board as requested, its core, what it needs and each menu's choices, the default first. A program sees
    // `board <fqbn> <needs> <name>` with every option in the FQBN, then `option <menu> <choice> <label>` per choice.
    void board_described(const selected_board& board, const board_requirements& requirements);

    // An item is a core or a data tool, such as `esp32:esp32-libs`. A program sees `start <item> <version> <bytes>`,
    // then `progress <item> <done> <bytes>` at most every 100 ms and at the end.
    void install_started(const std::string& item, const std::string& version, uint64_t length);
    void install_progress(const std::string& item, uint64_t done, uint64_t length, uint64_t now_ns);

    // A program sees `installed <item> <version>`, alone for an item installed already, or `removed <item> <version>`
    void installed(const std::string& item, const std::string& version, bool already_installed);
    void removed(const std::string& item, const std::string& version);

    // A program a board's recipes need that no Stellux package has installed. A program reading events sees
    // `needs <tool> <version>`, or `needs <file> -` for a compiler missing from its installed tool.
    void program_missing(const tool_dependency& tool);

    // `mcufw: <reason>` on stderr, or `failed <kind> <reason>`
    void failed(const failure& failure);

private:
    void event(const std::vector<std::string>& fields);

    bool        m_events;
    bool        m_terminal;          // stdout is a terminal, so counts update in place
    FILE*       m_out;
    FILE*       m_err;
    std::string m_progress_line;     // the start of the line that progress rewrites
    uint64_t    m_last_progress_ns = 0;
};

#endif // MCUFW_REPORT_HPP
