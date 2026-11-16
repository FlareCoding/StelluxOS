#ifndef MCUFW_REPORT_HPP
#define MCUFW_REPORT_HPP

// What a command tells its caller: sentences on stdout and failures on stderr, or with `--events` one fact per line on
// stdout in tab-separated fields, a tab or newline in a field written as a space. main prints usage errors itself.

#include "boards.hpp"
#include "failure.hpp"
#include "requirements.hpp"

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

    // `mcufw: <reason>` on stderr, or `failed <kind> <reason>`
    void failed(const failure& failure);

private:
    void event(const std::vector<std::string>& fields);

    bool  m_events;
    FILE* m_out;
    FILE* m_err;
};

#endif // MCUFW_REPORT_HPP
