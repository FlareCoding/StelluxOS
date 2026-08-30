#include "input.hpp"
#include "presenter.hpp"
#include "server.hpp"
#include "splash.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/font.h>

#include <csignal>
#include <cstdio>
#include <ctime>
#include <poll.h>
#include <unistd.h>
#include <vector>

constexpr const char* DM_PID_PATH = "/tmp/stlxdm.pid";

/* SIGHUP asks for a config reload, applied on the next wakeup */
static volatile sig_atomic_t g_reload = 0;

static void on_sighup(int) {
    g_reload = 1;
}

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

/* The pidfile lets the settings app deliver the reload signal */
static void write_pidfile() {
    FILE* f = fopen(DM_PID_PATH, "w");
    if (!f) {
        return;
    }

    fprintf(f, "%d\n", getpid());
    fclose(f);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    memcpy_presenter pres;
    if (pres.init() != 0) {
        printf("stlxdm: no framebuffer, exiting\r\n");
        return 1;
    }

    static stlxconf_t config;
    if (stlxconf_load(&config, STLXCONF_PATH) != 0) {
        printf("stlxdm: no config file, using defaults\r\n");
    }

    if (decor::init() != 0) {
        printf("stlxdm: chrome font unavailable\r\n");
        return 1;
    }

    /* Display state builds first, so the wallpaper decode and the
     * font opens are already paid when the splash asks for Enter */
    server srv;
    if (srv.init(&pres, &config) != 0) {
        printf("stlxdm: display init failed\r\n");
        pres.shutdown();
        return 1;
    }

    /* The splash owns the screen until Enter, before any client can
     * connect and before autostart spawns */
    splash_run(pres);

    if (srv.serve() != 0) {
        printf("stlxdm: failed to bind %s\r\n", SWP_SOCKET_PATH);
        pres.shutdown();
        return 1;
    }

    input inp;
    if (inp.init(pres.width(), pres.height(),
                 static_cast<uint64_t>(config.key_repeat_delay_ms) * 1000000ull,
                 static_cast<uint64_t>(config.key_repeat_interval_ms) * 1000000ull) != 0) {
        printf("stlxdm: no input devices, serving without input\r\n");
    }

    signal(SIGHUP, on_sighup);
    write_pidfile();

    printf("stlxdm: serving %ux%u\r\n", pres.width(), pres.height());

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

        /* The clock bounds every sleep at the next second rollover,
         * except behind the overlay where the bar is frozen */
        int64_t clock_ns = srv.clock_timeout_ns(now);
        if (clock_ns >= 0 && (next_ns < 0 || clock_ns < next_ns)) {
            next_ns = clock_ns;
        }

        if (srv.compose_pending()) {
            uint64_t deadline = last_compose_ns + COMPOSE_INTERVAL_NS;
            int64_t compose_ns = deadline > now
                               ? static_cast<int64_t>(deadline - now) : 0;
            if (next_ns < 0 || compose_ns < next_ns) {
                next_ns = compose_ns;
            }
        }
        int timeout_ms = next_ns < 0 ? -1 : static_cast<int>(next_ns / 1000000);

        int rc = poll(fds.data(), fds.size(), timeout_ms);

        /* A reload signal may be the very interruption poll saw */
        if (g_reload) {
            g_reload = 0;
            stlxconf_load(&config, STLXCONF_PATH);
            srv.reload_config();
            inp.set_repeat_rates(
                static_cast<uint64_t>(config.key_repeat_delay_ms) * 1000000ull,
                static_cast<uint64_t>(config.key_repeat_interval_ms) * 1000000ull);
        }

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
        srv.clock_tick();

        srv.pump(fds);

        now = now_ns();
        if (srv.compose_pending() &&
            now >= last_compose_ns + COMPOSE_INTERVAL_NS) {
            srv.compose_tick();
            last_compose_ns = now;
        }

        /* Everything this wakeup staged leaves before the next sleep */
        srv.flush_events();
    }
}
