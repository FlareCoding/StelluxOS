#ifndef MCUFW_SERIAL_PORTS_HPP
#define MCUFW_SERIAL_PORTS_HPP

// The serial ports in /dev/serial and the port a flash or monitor uses, never another one unless the board sets
// `upload.wait_for_upload_port`, whose port may come back under another name after the touch or the upload

#include "failure.hpp"

#include <cstdint>
#include <string>
#include <vector>

constexpr const char* SERIAL_DIRECTORY = "/dev/serial";

// arduino-cli's waits for a board's port: its bootloader's after the 1200 baud touch, and its own after an upload
constexpr uint64_t TOUCH_PORT_WAIT_NS  = 10000000000;
constexpr uint64_t UPLOAD_PORT_WAIT_NS = 5000000000;

// What a wait for a port has seen: the port's name, the listing when the wait began, the last listing, and whether the
// port has left
struct port_watch {
    std::string              name;
    std::vector<std::string> start;
    std::vector<std::string> last;
    bool                     has_left;
};

// The port names in /dev/serial, sorted
bool list_serial_ports(std::vector<std::string>& names, std::string& error);

// The path of the port typed by path or name, else sketch.yaml's, else the only one plugged in, `why` saying which.
// Fails as port on a name outside /dev/serial, or naming the ports plugged in when none or several fit.
bool choose_serial_port(const std::string& requested, const std::string& saved,
                        const std::vector<std::string>& present, std::string& path, std::string& why,
                        failure& failed);

// Opens the port at 1200 baud with DTR lowered and closes it, which asks a board that sets `upload.use_1200bps_touch`
// to enter its bootloader, then gives the board the half second arduino-cli gives it to reset
bool touch_at_1200_baud(const std::string& path, std::string& error);

// The name worth settling on in listing `now`: the watched port back after it left, else one neither the last listing
// nor the first held, so another board that re-plugs during the wait is never taken. Empty when there is none.
std::string port_to_settle_on(port_watch& watch, const std::vector<std::string>& now);

// The name the wait ends on once `settled` is listed a second after `candidate` appeared: the watched port when it left
// and is back, else `candidate` while it is still listed, empty to keep waiting
std::string settled_port(const port_watch& watch, const std::string& candidate,
                         const std::vector<std::string>& settled);

// Waits up to `timeout_ns` for the port at `path` to come back after it left or for a new port to stay, preferring
// `path`, naming its path in `found`, `path` at the deadline. Fails when a listing fails or a cancel arrives.
bool wait_for_port(const std::vector<std::string>& before, const std::string& path, uint64_t timeout_ns,
                   std::string& found, std::string& error);

#endif // MCUFW_SERIAL_PORTS_HPP
