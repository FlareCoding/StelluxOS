#ifndef MCUFW_BOARD_RELAY_HPP
#define MCUFW_BOARD_RELAY_HPP

// The board's output on its way to the terminal while a monitor decodes crashes: every byte relayed at once, each whole
// line given to the decoder, and the decoded lines printed as the decoder releases them

#include "crash_decoder.hpp"
#include "report.hpp"
#include "step_records.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// A longer line is still relayed whole, but only its start is searched for code addresses
constexpr size_t LINE_BYTES_MAX = 4096;

// The board's C compiler with its `gcc` changed to `addr2line`, as an AVR core's `avr-gcc` gives `avr-addr2line`,
// empty for a compiler named without `gcc`
std::string addr2line_beside(const std::string& compiler);

// addr2line's output for `addresses` in the ELF at `elf`, empty when it fails
using address_lookup = std::function<std::string(const std::string& elf, const std::vector<uint32_t>& addresses)>;

class board_relay {
public:
    // `elf` is the ELF as the decoder read it, so a build that replaces it during the session is noticed
    board_relay(crash_decoder& decoder, file_stamp elf, address_lookup look_up, reporter& report);

    // No bytes means the board fell silent or the session ended, so the decoder's `release_held` runs
    void take_bytes(std::string_view bytes);

private:
    void reload_rebuilt_elf();
    void decode_line();
    void print_ready_lines();

    crash_decoder& m_decoder;
    file_stamp     m_elf;
    address_lookup m_look_up;
    reporter&      m_report;
    std::string    m_line;  // the line the board is in the middle of
};

#endif // MCUFW_BOARD_RELAY_HPP
