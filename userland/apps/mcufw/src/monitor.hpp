#ifndef MCUFW_MONITOR_HPP
#define MCUFW_MONITOR_HPP

// Hands the terminal to `serial monitor`, so Stellux keeps one serial monitor. The `--dtr off` and `--rts off` the
// board asks for come before the words after `--`, and `serial` takes an option's last repeat, so those words win.

#include "crash_decoder.hpp"
#include "failure.hpp"
#include "properties.hpp"
#include "report.hpp"
#include "step_records.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

constexpr const char* SERIAL_PROGRAM = "/bin/serial";

struct monitor_request {
    std::string              port;
    uint32_t                 baud;
    bool                     lower_dtr;       // the board sets `serial.disableDTR`
    bool                     lower_rts;       // the board sets `serial.disableRTS`
    std::vector<std::string> serial_options;  // the words after `--`, passed through unchanged
    crash_decoder*           decoder;         // set to decode crash addresses, a state every line changes
    file_stamp               elf;             // the ELF as the decoder read it, so a rebuild is noticed
    std::string              addr2line;
};

// The request for `port`, lowering DTR and RTS when `board_properties` set `serial.disableDTR` and `serial.disableRTS`
monitor_request board_monitor_request(const std::string& port, uint32_t baud, const property_table& board_properties,
                                      const std::vector<std::string>& serial_options);

// `serial monitor`'s words: the port and `--baud`, then `--dtr off` and `--rts off` as asked, then the words after `--`
std::vector<std::string> serial_monitor_words(const monitor_request& request);

// Sets `request` up to decode crash addresses with the build `build_properties` describe, pointing it into `decoder`,
// which must outlive it, when the ELF and addr2line exist, and reports which ELF decodes or why none does
void prepare_crash_decoding(const property_table& build_properties, reporter& report,
                            std::optional<crash_decoder>& decoder, monitor_request& request);

// Fails when serial monitor cannot start, and gives its exit status otherwise, 1 when a signal ended it. With a
// decoder, mcufw relays the board's bytes at once and prints each decoded line the decoder releases.
bool run_monitor(const monitor_request& request, reporter& report, int& exit_status, failure& failed);

#endif // MCUFW_MONITOR_HPP
