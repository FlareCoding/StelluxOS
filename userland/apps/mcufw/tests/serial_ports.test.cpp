/* choose_serial_port and the wait for a port's return, run on the build machine: the port asked for, then
 * sketch.yaml's, then the only one, the failures that name the ports plugged in, and the listings a wait settles on */

#include "serial_ports.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// A listing during a wait and the listing after its settle
using listing_pair = std::pair<std::vector<std::string>, std::vector<std::string>>;

constexpr uint32_t MAX_REPORTS = 8;

static const std::vector<std::string> ONE_PORT = { "cp2102-0001" };
static const std::vector<std::string> TWO_PORTS = { "ch34x-0001", "cp2102-0001" };

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

static bool chooses(const std::string& requested, const std::string& saved, const std::vector<std::string>& present,
                    const std::string& expected_path, const std::string& expected_why) {
    std::string path;
    std::string why;
    failure failed;
    return choose_serial_port(requested, saved, present, path, why, failed) && path == expected_path &&
           why == expected_why;
}

static bool fails(const std::string& requested, const std::string& saved, const std::vector<std::string>& present,
                  const std::string& expected_reason) {
    std::string path;
    std::string why;
    failure failed;
    return !choose_serial_port(requested, saved, present, path, why, failed) && failed.kind == failure_kind::port &&
           failed.reason == expected_reason;
}

// The port a wait settles on over `listings`, each followed by the listing after the settle, empty while it waits
static std::string settles_on(const std::vector<std::string>& before,
                              const std::vector<listing_pair>& listings) {
    port_watch watch = { "ch34x-0001", before, before, false };
    watch.has_left = std::find(before.begin(), before.end(), watch.name) == before.end();
    for (const auto& [now, settled] : listings) {
        std::string candidate = port_to_settle_on(watch, now);
        std::string chosen = candidate.empty() ? std::string() : settled_port(watch, candidate, settled);
        if (!chosen.empty()) {
            return chosen;
        }

        watch.last = candidate.empty() ? now : settled;
    }

    return "";
}

static void check_port_waits() {
    const std::vector<std::string> ORIGINAL = { "ch34x-0001" };
    const std::vector<std::string> BOOTLOADER = { "acm-0001" };
    const std::vector<std::string> BOTH = { "acm-0001", "ch34x-0001" };

    check(settles_on(ORIGINAL, { { {}, {} }, { ORIGINAL, ORIGINAL } }) == "ch34x-0001",
          "settles on the port that left and came back");
    check(settles_on(ORIGINAL, { { BOOTLOADER, BOOTLOADER } }) == "acm-0001",
          "settles on a bootloader port that appeared and stayed");
    check(settles_on(ORIGINAL, { { BOOTLOADER, {} } }).empty(), "keeps waiting for a port gone again at the settle");
    check(settles_on(ORIGINAL, { { BOOTLOADER, {} }, { {}, {} }, { BOOTLOADER, BOOTLOADER } }) == "acm-0001",
          "takes a port gone at the settle once it returns");

    check(settles_on(ORIGINAL, { { BOTH, BOTH } }) == "acm-0001",
          "settles on a new port while the original never left");
    check(settles_on(ORIGINAL, { { {}, {} }, { BOOTLOADER, BOTH } }) == "ch34x-0001",
          "prefers the original back at the settle over a new port");

    check(settles_on(ORIGINAL, { { ORIGINAL, ORIGINAL }, { ORIGINAL, ORIGINAL } }).empty(),
          "keeps waiting while the original never left and nothing appeared");

    const std::vector<std::string> WITH_OTHER = { "ch34x-0001", "cp2102-0002" };
    const std::vector<std::string> OTHER = { "cp2102-0002" };
    const std::vector<std::string> OTHER_AND_BOOTLOADER = { "acm-0001", "cp2102-0002" };

    check(settles_on(WITH_OTHER, { { OTHER, OTHER }, { {}, {} }, { OTHER, OTHER } }).empty(),
          "never settles on another board that re-plugs during the wait");
    check(settles_on(WITH_OTHER, { { OTHER, OTHER }, { {}, {} }, { OTHER, OTHER }, { WITH_OTHER, WITH_OTHER } }) ==
              "ch34x-0001",
          "settles on the board's return under its own name while another board re-plugs");
    check(settles_on(WITH_OTHER, { { OTHER, OTHER }, { {}, {} }, { OTHER_AND_BOOTLOADER, OTHER_AND_BOOTLOADER } }) ==
              "acm-0001",
          "settles on the board's new name while another board re-plugs");

    check(TOUCH_PORT_WAIT_NS == 10000000000 && UPLOAD_PORT_WAIT_NS == 5000000000,
          "waits 10 s after the touch and 5 s after the upload, as arduino-cli does");
}

int main() {
    check(chooses("", "", ONE_PORT, "/dev/serial/cp2102-0001", "the only serial port"), "takes the only port");
    check(chooses("cp2102-0001", "", TWO_PORTS, "/dev/serial/cp2102-0001", "as asked"), "takes a port by name");
    check(chooses("/dev/serial/ch34x-0001", "/dev/serial/cp2102-0001", TWO_PORTS, "/dev/serial/ch34x-0001", "as asked"),
          "takes the port asked for over sketch.yaml's");
    check(chooses("", "/dev/serial/cp2102-0001", TWO_PORTS, "/dev/serial/cp2102-0001", "kept in sketch.yaml"),
          "takes sketch.yaml's port among several");

    check(fails("", "", TWO_PORTS,
                "the serial ports plugged in are ch34x-0001 and cp2102-0001, so name one with --port"),
          "stops when several ports are plugged in");
    check(fails("", "", {}, "no serial port is plugged in"), "stops when no port is plugged in");
    check(fails("cp2102-0002", "", ONE_PORT,
                "cp2102-0002 is not plugged in, and the serial port plugged in is cp2102-0001"),
          "stops when the port asked for is not plugged in");
    check(fails("", "/dev/serial/cp2102-0002", ONE_PORT,
                "sketch.yaml's port cp2102-0002 is not plugged in, and the serial port plugged in is cp2102-0001"),
          "never moves on from sketch.yaml's port");
    check(fails("", "cp2102-0002", {}, "sketch.yaml's port cp2102-0002 is not plugged in, and no serial port is "
                                       "plugged in"),
          "names that no port is plugged in");

    check(fails("/dev/ttyUSB0", "", ONE_PORT, "/dev/ttyUSB0 is not a serial port in /dev/serial"),
          "refuses a path outside /dev/serial");
    check(fails("/dev/serial/../console", "", ONE_PORT, "/dev/serial/../console is not a serial port in /dev/serial"),
          "refuses a path that leaves /dev/serial");
    check(fails("/dev/serial/", "", ONE_PORT, "/dev/serial/ is not a serial port in /dev/serial"),
          "refuses /dev/serial itself");
    check(fails("..", "", ONE_PORT, ".. is not a serial port in /dev/serial") &&
              fails("/dev/serial/.", "", ONE_PORT, "/dev/serial/. is not a serial port in /dev/serial"),
          "refuses the names of folders");

    check_port_waits();
    if (g_failures != 0) {
        printf("serial_ports: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("serial_ports: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
