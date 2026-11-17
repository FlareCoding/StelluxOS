#ifndef MCUFW_REPORT_HPP
#define MCUFW_REPORT_HPP

// What a command tells its caller: sentences on stdout, failures and tools' output on stderr, or with `--events` one
// fact per line in tab-separated fields, a tab or newline in a field written as a space. main prints usage errors.

#include "boards.hpp"
#include "compiler_output.hpp"
#include "failure.hpp"
#include "libraries.hpp"
#include "library_hint.hpp"
#include "requirements.hpp"
#include "sizes.hpp"
#include "sketch.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

struct build_result;

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

    // An item is a core, a data tool such as `esp32:esp32-libs`, or a library such as `Adafruit GFX Library`. A program
    // sees `start <item> <version> <bytes>`, then `progress <item> <done> <bytes>` at most every 100 ms and at the end.
    void install_started(const std::string& item, const std::string& version, uint64_t length);
    void install_progress(const std::string& item, uint64_t done, uint64_t length, uint64_t now_ns);

    // A program sees `installed <item> <version>`, alone for an item installed already, or `removed <item> <version>`
    void installed(const std::string& item, const std::string& version, bool already_installed);
    void removed(const std::string& item, const std::string& version);

    // A program a board's recipes need that no Stellux package has installed. A program reading events sees
    // `needs <tool> <version>`, or `needs <file> -` for a compiler missing from its installed tool.
    void program_missing(const tool_dependency& tool);

    // Something the run went on without, `mcufw: <reason>` on stderr, or `warning <reason>`
    void warned(const std::string& reason);

    // A build's facts. A person sees the `Board` and `Core` lines. A program sees `sketch <name> <folder>`,
    // `board <fqbn> <name>` with every option in effect in the FQBN, and `core compile <archive>`.
    void sketch_found(const sketch& sketch);
    void board_selected(const selected_board& board, bool saved);
    void core_compiling(const installed_core& core, const std::string& archive, size_t files);

    // The core cache's archive a build links instead of compiling the core, `core cached <archive>` for a program
    void core_cached(const std::string& archive);

    // The libraries the sketch uses, `library <name> <version> <folder>` each for a program
    void libraries_found(const std::vector<const library*>& libraries);

    // The library that provides a header no installed library does, and the command installing it
    void library_suggested(const std::string& header, const library_suggestion& suggestion);

    // Warns, as arduino-cli does, about a library whose architectures leave out the board's `architectures`
    void library_incompatible(const library& library, const std::vector<std::string>& architectures);

    // A count updated in place on a terminal at most every 100 ms, and a line once it reaches the total. A program
    // sees `progress <stage> <done> <total>` at most every 100 ms and at the total.
    void progress(std::string_view stage, uint64_t done, uint64_t total, uint64_t now_ns);

    // How many of the build's `total` compiles must run, which its compile count counts up to and a stage may change.
    // A person's last count reads `Compiling 1 of 75, Lamp.ino changed`, with the first reason that names a change.
    void compiles_needed(size_t needed, size_t total, const std::string& first_reason);

    // With `--verbose`, the command with a word holding a space, tab, `"` or `\` quoted as arduino-cli prints it. A
    // program sees no commands.
    void command_started(const std::vector<std::string>& words);

    // A tool's output as it printed it, or a warning mcufw gives about the tool's step. A program sees
    // `log <stage> <line>` for each of its lines.
    void tool_output(std::string_view stage, std::string_view text);

    // `problem <severity> <file> <line> <column> <message>`, which only a program sees, since a person reads the
    // compiler's own words
    void problem(const compiler_problem& problem);

    // `Flash` and `RAM` lines with each share of the board's maximum, or `size flash|ram <used> <maximum>`, with `-`
    // for a maximum the board does not state. No RAM line comes without the core's data regex.
    void size_checked(const program_size& size, uint64_t flash_max, uint64_t ram_max);

    // `Built <image> in <time>`, or `built <image>`
    void built(const build_result& result, uint64_t elapsed_ns);

    // The port a flash uses and why it was chosen. A program sees `port <path>`.
    void port_chosen(const std::string& path, const std::string& why);

    // The upload tool's output, which a person sees as printed. A program sees `log flash <line>` per nonempty line
    // ended by CR or LF, and `progress flash <percent> 100` after each line naming a percentage.
    void upload_output(std::string_view bytes);

    // That the upload finished and how long the flash took, the touch and the waits included. A program sees
    // `flashed <port>`, naming the port the board answers on after the upload.
    void flashed(const std::string& port, uint64_t elapsed_ns);

    // The ELF a monitor decodes crash addresses with, or why it decodes none, either of which may come while `serial`
    // keeps the terminal raw, so each ends in CR LF. The monitor prints no events.
    void decoding(const std::string& elf);
    void decoding_off(const std::string& why);

    // The board's bytes as `serial monitor` relayed them, and the decoded lines after them, each ending in CR LF since
    // `serial` keeps the terminal raw
    void board_output(std::string_view bytes);
    void decoded_lines(const std::vector<std::string>& lines);

    // `mcufw: <reason>` on stderr, or `failed <kind> <reason>`
    void failed(const failure& failure);

private:
    void event(const std::vector<std::string>& fields);
    void labeled(std::string_view label, const std::string& text);
    void hide_progress();
    void end_upload_line();

    bool        m_events;
    bool        m_terminal;          // stdout is a terminal, so counts update in place
    FILE*       m_out;
    FILE*       m_err;
    std::string m_progress_line;     // the start of the line that progress rewrites
    uint64_t    m_last_progress_ns = 0;
    std::string m_shown_count;       // a build's count shown in place, empty when none is
    size_t      m_compiles_needed = 0;
    size_t      m_compiles_total = 0;
    std::string m_compile_reason;
    std::string m_upload_line;       // the upload tool's line not yet ended, for a program's events
};

// The monotonic clock in nanoseconds, which progress counts and a build's time are measured against
uint64_t monotonic_clock_ns();

#endif // MCUFW_REPORT_HPP
