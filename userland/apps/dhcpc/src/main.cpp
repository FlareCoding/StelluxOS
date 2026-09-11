#include "client.hpp"

#include <stlx/net.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

constexpr const char* NET_EVENTS_PATH = "/dev/net/events";

/* SIGTERM is the orderly shutdown, the leases go back before the process does */
static volatile sig_atomic_t g_shutdown = 0;

static void on_sigterm(int) {
    g_shutdown = 1;
}

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

/* Named interfaces when any were given, otherwise every interface that is not loopback */
static bool wanted(const stlx_ifinfo& info, const std::vector<const char*>& names) {
    if (names.empty()) {
        return !(info.flags & STLX_IFF_LOOPBACK);
    }

    for (const char* name : names) {
        if (strcmp(name, info.name) == 0) {
            return true;
        }
    }

    return false;
}

static bool has_carrier(const stlx_net_status& status, const char* name) {
    for (uint32_t i = 0; i < status.if_count; i++) {
        if (strcmp(status.interfaces[i].name, name) == 0) {
            return (status.interfaces[i].flags & STLX_IFF_RUNNING) != 0;
        }
    }

    return false;
}

static int poll_timeout_ms(uint64_t deadline_ns, uint64_t now) {
    if (deadline_ns == DHCP_NO_DEADLINE) {
        return -1;
    }

    if (deadline_ns <= now) {
        return 0;
    }

    return static_cast<int>((deadline_ns - now + 999999) / 1000000);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool verbose = false;
    uint32_t lease_cap_s = 0;
    std::vector<const char*> names;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc && atoi(argv[i + 1]) > 0) {
            lease_cap_s = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (argv[i][0] == '-') {
            printf("Usage: dhcpc [-v] [-l seconds] [interface...]\r\n");
            return 1;
        } else {
            names.push_back(argv[i]);
        }
    }

    stlx_net_status status;
    if (stlx_net_get_status(&status) != 0) {
        printf("dhcpc: cannot list interfaces: %s\r\n", strerror(errno));
        return 1;
    }

    std::vector<std::unique_ptr<dhcp_client>> clients;
    for (uint32_t i = 0; i < status.if_count; i++) {
        const stlx_ifinfo& info = status.interfaces[i];
        if (!wanted(info, names)) {
            continue;
        }

        auto client = std::make_unique<dhcp_client>();
        bool link_up = (info.flags & STLX_IFF_RUNNING) != 0;
        if (client->open(info.name, info.mac, verbose, lease_cap_s, link_up) != 0) {
            printf("dhcpc: %s: %s\r\n", info.name, strerror(errno));
            continue;
        }

        clients.push_back(std::move(client));
    }

    if (clients.empty()) {
        printf("dhcpc: no interface to configure\r\n");
        return 1;
    }

    signal(SIGTERM, on_sigterm);

    int events_fd = open(NET_EVENTS_PATH, O_RDONLY);
    if (events_fd < 0) {
        printf("dhcpc: cannot watch %s: %s\r\n", NET_EVENTS_PATH, strerror(errno));
        return 1;
    }

    std::vector<pollfd> fds(clients.size() + 1);
    while (true) {
        uint64_t now = now_ns();
        uint64_t next_deadline = DHCP_NO_DEADLINE;
        for (size_t i = 0; i < clients.size(); i++) {
            fds[i] = {clients[i]->fd(), POLLIN, 0};
            next_deadline = std::min(next_deadline, clients[i]->deadline_ns());
        }

        fds[clients.size()] = {events_fd, POLLIN, 0};

        if (poll(fds.data(), fds.size(), poll_timeout_ms(next_deadline, now)) < 0 && errno != EINTR) {
            printf("dhcpc: poll: %s\r\n", strerror(errno));
            return 1;
        }

        if (g_shutdown) {
            for (auto& client : clients) {
                client->release();
            }

            return 0;
        }

        now = now_ns();
        if (fds[clients.size()].revents & POLLIN) {
            char generation[32];
            (void)read(events_fd, generation, sizeof(generation));
            if (stlx_net_get_status(&status) == 0) {
                for (auto& client : clients) {
                    client->on_link(has_carrier(status, client->iface()), now);
                }
            }
        }

        for (size_t i = 0; i < clients.size(); i++) {
            if (fds[i].revents & POLLIN) {
                clients[i]->on_readable(now);
            }

            if (now >= clients[i]->deadline_ns()) {
                clients[i]->on_timeout(now);
            }
        }
    }
}
