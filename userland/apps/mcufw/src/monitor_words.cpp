#include "monitor.hpp"

monitor_request board_monitor_request(const std::string& port, uint32_t baud, const property_table& board_properties,
                                      const std::vector<std::string>& serial_options) {
    return { port, baud, property_is_true(board_properties, "serial.disableDTR"),
             property_is_true(board_properties, "serial.disableRTS"), serial_options, nullptr, {}, "" };
}

std::vector<std::string> serial_monitor_words(const monitor_request& request) {
    std::vector<std::string> words = { SERIAL_PROGRAM, "monitor", request.port, "--baud",
                                       std::to_string(request.baud) };
    if (request.lower_dtr) {
        words.insert(words.end(), { "--dtr", "off" });
    }

    if (request.lower_rts) {
        words.insert(words.end(), { "--rts", "off" });
    }

    words.insert(words.end(), request.serial_options.begin(), request.serial_options.end());

    return words;
}
