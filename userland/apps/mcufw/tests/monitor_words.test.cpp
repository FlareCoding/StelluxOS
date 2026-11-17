/* board_monitor_request and serial_monitor_words, run on the build machine: the DTR and RTS levels each board key asks
 * for, and the words after `--` kept exactly and last so they win */

#include "monitor.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

static const std::string PORT = "/dev/serial/cp2102-0001";

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static property_table board_with(const char* text) {
    property_table properties;
    std::string error;
    if (!parse_properties(text, properties, error)) {
        printf("  %s\n", error.c_str());
    }

    return properties;
}

static void check_levels() {
    monitor_request request = board_monitor_request(PORT, 115200, board_with(""), {});
    check(!request.lower_dtr && !request.lower_rts && request.port == PORT && request.baud == 115200,
          "lowers neither line for a board without the keys");

    request = board_monitor_request(PORT, 115200, board_with("serial.disableDTR=true\n"), {});
    check(request.lower_dtr && !request.lower_rts, "lowers DTR alone for serial.disableDTR");

    request = board_monitor_request(PORT, 115200, board_with("serial.disableRTS=true\n"), {});
    check(!request.lower_dtr && request.lower_rts, "lowers RTS alone for serial.disableRTS");
}

static void check_words() {
    std::vector<std::string> plain = { "/bin/serial", "monitor", PORT, "--baud", "115200" };
    check(serial_monitor_words({ PORT, 115200, false, false, {}, nullptr, {}, "" }) == plain,
          "starts serial monitor on the port at mcufw's rate");

    std::vector<std::string> options = { "--", "--events", "--baud", "9600" };
    std::vector<std::string> lowered = { "/bin/serial", "monitor", PORT, "--baud", "115200", "--dtr", "off",
                                         "--rts",       "off",     "--", "--events", "--baud", "9600" };
    check(serial_monitor_words({ PORT, 115200, true, true, options, nullptr, {}, "" }) == lowered,
          "puts the board's levels after the rate and the words after -- last, unchanged");

    std::vector<std::string> dtr_only = { "/bin/serial", "monitor", PORT, "--baud", "115200", "--dtr", "off" };
    check(serial_monitor_words({ PORT, 115200, true, false, {}, nullptr, {}, "" }) == dtr_only,
          "passes only the level asked for");
}

int main() {
    check_levels();
    check_words();

    if (g_failures != 0) {
        printf("monitor_words: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("monitor_words: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
