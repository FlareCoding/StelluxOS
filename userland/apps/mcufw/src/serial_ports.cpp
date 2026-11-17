#include "serial_ports.hpp"

#include "cancel.hpp"
#include "files.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>

constexpr std::string_view SERIAL_PATH_PREFIX = "/dev/serial/";
constexpr uint64_t         NS_PER_SECOND      = 1000000000;
constexpr uint64_t         TOUCH_RESET_NS     = 500000000;
constexpr uint64_t         PORT_POLL_NS       = 250000000;

// A port that appears must still be there this long after, since some bootloaders' ports come and go before settling
constexpr uint64_t PORT_SETTLE_NS = 1000000000;

static uint64_t monotonic_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * NS_PER_SECOND + static_cast<uint64_t>(now.tv_nsec);
}

// Sleeps for `duration_ns`, or less when a signal arrives, so a cancel is noticed at once
static void sleep_ns(uint64_t duration_ns) {
    timespec duration = { static_cast<time_t>(duration_ns / NS_PER_SECOND),
                          static_cast<long>(duration_ns % NS_PER_SECOND) };
    nanosleep(&duration, nullptr);
}

static bool is_listed(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// The name a port is given by, bare or after /dev/serial/, or an empty string when it names no port there
static std::string port_name(std::string_view given) {
    if (given.starts_with(SERIAL_PATH_PREFIX)) {
        given.remove_prefix(SERIAL_PATH_PREFIX.size());
    }

    bool is_name = !given.empty() && given != "." && given != ".." && given.find('/') == std::string_view::npos;

    return is_name ? std::string(given) : std::string();
}

static std::string describe_present(const std::vector<std::string>& present) {
    if (present.empty()) {
        return "no serial port is plugged in";
    }

    std::string names;
    for (size_t index = 0; index < present.size(); index++) {
        names += index == 0 ? "" : index + 1 == present.size() ? " and " : ", ";
        names += present[index];
    }

    return (present.size() == 1 ? "the serial port plugged in is " : "the serial ports plugged in are ") + names;
}

static bool choose_given_port(const std::string& given, const std::string& described_as,
                              const std::vector<std::string>& present, std::string& path, failure& failed) {
    std::string name = port_name(given);
    if (name.empty()) {
        failed = { failure_kind::port, described_as + given + " is not a serial port in /dev/serial" };
        return false;
    }

    if (!is_listed(present, name)) {
        failed = { failure_kind::port, described_as + name + " is not plugged in, and " + describe_present(present) };
        return false;
    }

    path = join_path(SERIAL_DIRECTORY, name);

    return true;
}

bool list_serial_ports(std::vector<std::string>& names, std::string& error) {
    return list_directory(SERIAL_DIRECTORY, names, error);
}

bool choose_serial_port(const std::string& requested, const std::string& saved,
                        const std::vector<std::string>& present, std::string& path, std::string& why,
                        failure& failed) {
    if (!requested.empty()) {
        why = "as asked";
        return choose_given_port(requested, "", present, path, failed);
    }

    if (!saved.empty()) {
        why = "kept in sketch.yaml";
        return choose_given_port(saved, "sketch.yaml's port ", present, path, failed);
    }

    if (present.size() == 1) {
        path = join_path(SERIAL_DIRECTORY, present[0]);
        why = "the only serial port";
        return true;
    }

    std::string reason = describe_present(present);
    failed = { failure_kind::port, present.empty() ? reason : reason + ", so name one with --port" };
    return false;
}

bool touch_at_1200_baud(const std::string& path, std::string& error) {
    int fd = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path + ": " + strerror(errno);
        return false;
    }

    termios settings;
    int lowered = TIOCM_DTR;
    bool is_touched = tcgetattr(fd, &settings) == 0 && cfsetispeed(&settings, B1200) == 0 &&
                      cfsetospeed(&settings, B1200) == 0 && tcsetattr(fd, TCSANOW, &settings) == 0 &&
                      ioctl(fd, TIOCMBIC, &lowered) == 0;
    int touch_error = errno;
    close(fd);
    if (!is_touched) {
        error = "cannot set " + path + " to 1200 baud with DTR lowered: " + strerror(touch_error);
        return false;
    }

    sleep_ns(TOUCH_RESET_NS);

    return true;
}

std::string port_to_settle_on(port_watch& watch, const std::vector<std::string>& now) {
    bool is_back = watch.has_left && is_listed(now, watch.name);
    watch.has_left = watch.has_left || !is_listed(now, watch.name);
    if (is_back) {
        return watch.name;
    }

    auto appeared = std::find_if(now.begin(), now.end(), [&watch](const std::string& listed) {
        return !is_listed(watch.last, listed) && !is_listed(watch.start, listed);
    });

    return appeared == now.end() ? std::string() : *appeared;
}

std::string settled_port(const port_watch& watch, const std::string& candidate,
                         const std::vector<std::string>& settled) {
    if (watch.has_left && is_listed(settled, watch.name)) {
        return watch.name;
    }

    return is_listed(settled, candidate) ? candidate : std::string();
}

bool wait_for_port(const std::vector<std::string>& before, const std::string& path, uint64_t timeout_ns,
                   std::string& found, std::string& error) {
    std::string name = file_name(path);
    port_watch watch = { name, before, before, !is_listed(before, name) };
    uint64_t deadline = monotonic_ns() + timeout_ns;
    while (monotonic_ns() < deadline) {
        if (cancel_requested()) {
            error = "the wait for " + path + " was cancelled";
            return false;
        }

        std::vector<std::string> now;
        if (!list_serial_ports(now, error)) {
            return false;
        }

        std::string candidate = port_to_settle_on(watch, now);
        if (!candidate.empty()) {
            sleep_ns(PORT_SETTLE_NS);

            std::vector<std::string> settled;
            if (!list_serial_ports(settled, error)) {
                return false;
            }

            std::string chosen = settled_port(watch, candidate, settled);
            if (!chosen.empty()) {
                found = chosen == name ? path : join_path(SERIAL_DIRECTORY, chosen);
                return true;
            }

            now = settled;
        }

        watch.last = now;
        sleep_ns(PORT_POLL_NS);
    }

    found = path;

    return true;
}
