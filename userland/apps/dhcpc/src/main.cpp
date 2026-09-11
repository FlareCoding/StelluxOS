#include "client.hpp"

#include <stlx/net.h>

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

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

static int poll_timeout_ms(uint64_t deadline_ns, uint64_t now) {
    if (deadline_ns == UINT64_MAX) {
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
    std::vector<const char*> names;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (argv[i][0] == '-') {
            printf("Usage: dhcpc [-v] [interface...]\r\n");
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
        if (client->open(info.name, info.mac, verbose) != 0) {
            printf("dhcpc: %s: %s\r\n", info.name, strerror(errno));
            continue;
        }

        clients.push_back(std::move(client));
    }

    if (clients.empty()) {
        printf("dhcpc: no interface to configure\r\n");
        return 1;
    }

    std::vector<pollfd> fds(clients.size());
    while (true) {
        uint64_t now = now_ns();
        uint64_t next_deadline = UINT64_MAX;
        for (size_t i = 0; i < clients.size(); i++) {
            fds[i] = {clients[i]->fd(), POLLIN, 0};
            next_deadline = std::min(next_deadline, clients[i]->deadline_ns());
        }

        if (poll(fds.data(), fds.size(), poll_timeout_ms(next_deadline, now)) < 0 && errno != EINTR) {
            printf("dhcpc: poll: %s\r\n", strerror(errno));
            return 1;
        }

        now = now_ns();
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
