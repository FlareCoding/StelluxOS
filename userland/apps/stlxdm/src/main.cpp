#include "input.hpp"
#include "screen.hpp"
#include "server.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <poll.h>
#include <vector>

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    screen scr;
    if (scr.init() != 0) {
        printf("stlxdm: no framebuffer, exiting\r\n");
        return 1;
    }

    /* Composition happens here, never in the scanout mapping */
    uint8_t* backbuffer =
        static_cast<uint8_t*>(malloc((size_t)scr.width * scr.height * 4));
    if (!backbuffer) {
        printf("stlxdm: backbuffer allocation failed\r\n");
        scr.shutdown();
        return 1;
    }

    server srv;
    if (srv.init(&scr) != 0) {
        printf("stlxdm: failed to bind %s\r\n", SWP_SOCKET_PATH);
        scr.shutdown();
        return 1;
    }

    input inp;
    if (inp.init(&scr) != 0) {
        printf("stlxdm: no input devices, serving without input\r\n");
    }

    printf("stlxdm: serving %ux%u\r\n", scr.width, scr.height);

    constexpr uint64_t COMPOSE_INTERVAL_NS = 16666667ull;
    uint64_t last_compose_ns = 0;

    std::vector<pollfd> fds;
    while (true) {
        fds.clear();
        srv.collect_fds(fds);

        size_t kbd_slot = fds.size();
        if (inp.kbd_fd() >= 0) {
            fds.push_back({ inp.kbd_fd(), POLLIN, 0 });
        }
        size_t mouse_slot = fds.size();
        if (inp.mouse_fd() >= 0) {
            fds.push_back({ inp.mouse_fd(), POLLIN, 0 });
        }

        /* The timeout is the sooner of key repeat and, when damage is
         * pending, the paced compose deadline. Idle blocks forever. */
        uint64_t now = now_ns();
        int64_t next_ns = inp.repeat_timeout_ns(now);
        if (srv.compose_pending()) {
            uint64_t deadline = last_compose_ns + COMPOSE_INTERVAL_NS;
            int64_t compose_ns = deadline > now
                               ? (int64_t)(deadline - now) : 0;
            if (next_ns < 0 || compose_ns < next_ns) {
                next_ns = compose_ns;
            }
        }
        int timeout_ms = next_ns < 0 ? -1 : (int)(next_ns / 1000000);

        int rc = poll(fds.data(), fds.size(), timeout_ms);
        if (rc < 0) {
            continue;
        }

        if (inp.kbd_fd() >= 0 && (fds[kbd_slot].revents & POLLIN)) {
            inp.pump_kbd(srv);
        }
        if (inp.mouse_fd() >= 0 && (fds[mouse_slot].revents & POLLIN)) {
            inp.pump_mouse(srv);
        }
        inp.pump_repeat(srv, now_ns());

        srv.pump(fds);

        now = now_ns();
        if (srv.compose_pending() &&
            now >= last_compose_ns + COMPOSE_INTERVAL_NS) {
            srv.compose_tick(scr, backbuffer);
            last_compose_ns = now;
        }
    }
}
