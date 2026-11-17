#ifndef MCUFW_MONITOR_HPP
#define MCUFW_MONITOR_HPP

// Hands the terminal to `serial monitor`, so Stellux keeps one serial monitor. The `--dtr off` and `--rts off` the
// board asks for come before the words after `--`, and `serial` takes an option's last repeat, so those words win.

#include "failure.hpp"
#include "properties.hpp"

#include <cstdint>
#include <string>
#include <vector>

constexpr const char* SERIAL_PROGRAM = "/bin/serial";

struct monitor_request {
    std::string              port;
    uint32_t                 baud;
    bool                     lower_dtr;       // the board sets `serial.disableDTR`
    bool                     lower_rts;       // the board sets `serial.disableRTS`
    std::vector<std::string> serial_options;  // the words after `--`, passed through unchanged
};

// The request for `port`, lowering DTR and RTS when `board_properties` set `serial.disableDTR` and `serial.disableRTS`
monitor_request board_monitor_request(const std::string& port, uint32_t baud, const property_table& board_properties,
                                      const std::vector<std::string>& serial_options);

// `serial monitor`'s words: the port and `--baud`, then `--dtr off` and `--rts off` as asked, then the words after `--`
std::vector<std::string> serial_monitor_words(const monitor_request& request);

// Fails when serial monitor cannot start, and gives its exit status otherwise, 1 when a signal ended it
bool run_monitor(const monitor_request& request, int& exit_status, failure& failed);

#endif // MCUFW_MONITOR_HPP
