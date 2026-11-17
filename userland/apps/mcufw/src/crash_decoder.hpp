#ifndef MCUFW_CRASH_DECODER_HPP
#define MCUFW_CRASH_DECODER_HPP

// Decodes the code addresses in each line a board prints, as ESP-IDF's monitor does, holding the decoded lines until
// an `ELF file SHA256:` line releases them when its digits start the ELF's SHA-256, else drops them and stops decoding.

#include "elf.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Half a second with no byte from the board ends what it was printing, so the monitor calls `release_held`
constexpr uint64_t SILENCE_BEFORE_RELEASE_NS = 500000000;

// The code addresses in `line` in order, each `0x` and eight hex digits inside an executable section
std::vector<uint32_t> find_code_addresses(std::string_view line, const std::vector<address_range>& ranges);

// `addr2line -fiaC` output as `<address>: <function> at <file>:<line>` lines, then ` (inlined by) <function> at
// <file>:<line>` per function the code was inlined into, leaving out addresses it cannot place
std::vector<std::string> format_addr2line_output(std::string_view output);

class crash_decoder {
public:
    crash_decoder(std::vector<address_range> ranges, std::string elf_sha256);

    // Takes a whole line the board printed, and returns the addresses in it not looked up before in this session.
    // When it returns any, the monitor must hand their addr2line output to `take_lookup` before the next line.
    std::vector<uint32_t> take_line(std::string_view line);

    void take_lookup(const std::vector<uint32_t>& addresses, std::string_view addr2line_output);

    // Once the board fell silent or the session ended, releases the held lines unchecked while no hash line came this
    // session, and drops them after one did, since such a board's report without one cannot be checked
    void release_held();

    // Starts over with a rebuilt ELF's ranges and hash, waiting for a hash line again
    void reload(std::vector<address_range> ranges, std::string elf_sha256);

    // Stops decoding until a reload, dropping the held lines, for an ELF that changed and cannot be read yet
    void stop();

    // The lines ready to print now, in order
    std::vector<std::string> take_ready_lines();

    // False from a hash line showing that the board runs another build than the ELF, which dropped the held lines,
    // until a reload
    bool decoding() const;

private:
    void hold_pending_lines();

    std::vector<address_range>                   m_ranges;
    std::string                                  m_elf_sha256;
    std::map<uint32_t, std::vector<std::string>> m_decoded;  // each address's lines, once looked up
    std::vector<uint32_t>                        m_pending;  // the code addresses of the line awaiting its lookup
    std::vector<std::string>                     m_held;
    std::vector<std::string>                     m_ready;
    bool                                         m_decoding = true;
    bool                                         m_has_seen_hash_line = false;  // in this session, reloads included
};

#endif // MCUFW_CRASH_DECODER_HPP
