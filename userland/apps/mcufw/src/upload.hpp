#ifndef MCUFW_UPLOAD_HPP
#define MCUFW_UPLOAD_HPP

// The board's upload recipe as arduino-cli runs it for a serial port: the tool's `upload.pattern`, with the tool's keys
// expanding without their prefix

#include "properties.hpp"

#include <string>
#include <string_view>
#include <vector>

// The upload command for the build's properties and `port`, with the port, verbosity and verification keys arduino-cli
// sets, and empty when the tool's pattern is, which uploads nothing in arduino-cli either
bool upload_words(const property_table& build_properties, const std::string& port, bool verbose,
                  std::vector<std::string>& words, std::string& error);

// Whether the board sets `upload.use_1200bps_touch` for the 1200 baud touch, and `upload.wait_for_upload_port` to wait
// for its port after the touch and the upload
bool wants_1200_baud_touch(const property_table& board_properties);
bool waits_for_upload_port(const property_table& board_properties);

// The last percentage in one line the upload tool printed, a line ending at CR or LF, esptool's `100.0%` and avrdude's
// `100%` alike, or -1
int last_percentage(std::string_view line);

#endif // MCUFW_UPLOAD_HPP
