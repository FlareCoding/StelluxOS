#include "input.hpp"
#include "presenter.hpp"
#include "server.hpp"
#include "splash.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/font.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <poll.h>
#include <unistd.h>
#include <vector>

constexpr const char* DM_PID_PATH = "/tmp/stlxdm.pid";

constexpr uint64_t NS_PER_SECOND = 1000000000ull;
constexpr uint64_t MILLIHERTZ_PER_HERTZ = 1000;

/* The rate stlxdm composes at when the screen's rate is unknown */
constexpr uint64_t DEFAULT_REFRESH_MILLIHERTZ = 60000;

/* How long stlxdm waits before asking again for display buffers while
 * another program controls the display's buffers */
constexpr long DISPLAY_BUSY_RETRY_NS = 100000000;

/* SIGHUP asks for a config reload, applied on the next wakeup */
static volatile sig_atomic_t g_reload = 0;

static void on_sighup(int) {
    g_reload = 1;
}

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * NS_PER_SECOND + static_cast<uint64_t>(ts.tv_nsec);
}

/* The time between refreshes at `refresh_millihertz`, which must not
 * be 0, to the nearest nanosecond */
static uint64_t refresh_interval_ns(uint64_t refresh_millihertz) {
    return (NS_PER_SECOND * MILLIHERTZ_PER_HERTZ + refresh_millihertz / 2) / refresh_millihertz;
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

/* The main file over the defaults, then the drop-in files over that */
static void load_config(stlxconf_t& config) {
    if (stlxconf_load(&config, STLXCONF_PATH) != 0) {
        printf("stlxdm: no config file, using defaults\r\n");
    }

    if (stlxconf_load_drop_ins(&config, STLXCONF_DROP_IN_DIR) != 0) {
        printf("stlxdm: could not read %s\r\n", STLXCONF_DROP_IN_DIR);
    }
}

/* The flip presenter, waited for while another program controls the display's buffers since a
 * frame drawn into the framebuffer would not show, else the memcpy presenter, else null */
static presenter* open_presenter(flip_presenter& flip, memcpy_presenter& fallback) {
    bool reported_busy = false;
    while (true) {
        int rc = flip.init();
        if (rc == 0) {
            printf("stlxdm: presenting through display buffers\r\n");
            return &flip;
        }

        if (rc != -EBUSY) {
            break;
        }

        if (!reported_busy) {
            printf("stlxdm: waiting for the program controlling the display's buffers to close /dev/gfxfb\r\n");
            reported_busy = true;
        }

        timespec retry = { 0, DISPLAY_BUSY_RETRY_NS };
        nanosleep(&retry, nullptr);
    }

    if (fallback.init() != 0) {
        return nullptr;
    }

    printf("stlxdm: drawing into the framebuffer\r\n");

    return &fallback;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    flip_presenter flip;
    memcpy_presenter fallback;
    presenter* pres = open_presenter(flip, fallback);
    if (!pres) {
        printf("stlxdm: no framebuffer, exiting\r\n");
        return 1;
    }

    static stlxconf_t config;
    load_config(config);

    if (decor::init() != 0) {
        printf("stlxdm: chrome font unavailable\r\n");
        return 1;
    }

    /* Display state builds first, so the wallpaper decode and the
     * font opens are already paid when the splash asks for Enter */
    server srv;
    if (srv.init(pres, &config) != 0) {
        printf("stlxdm: display init failed\r\n");
        pres->shutdown();
        return 1;
    }

    /* The splash owns the screen until Enter, before any client can
     * connect and before autostart spawns */
    splash_run(*pres);

    if (srv.serve() != 0) {
        printf("stlxdm: failed to bind %s\r\n", SWP_SOCKET_PATH);
        pres->shutdown();
        return 1;
    }

    input inp;
    if (inp.init(pres->width(), pres->height(),
                 static_cast<uint64_t>(config.key_repeat_delay_ms) * 1000000ull,
                 static_cast<uint64_t>(config.key_repeat_interval_ms) * 1000000ull) != 0) {
        printf("stlxdm: no input devices, serving without input\r\n");
    }

    signal(SIGHUP, on_sighup);
    write_pidfile();

    /* Composition waits for the last present to end and runs at most once
     * per refresh, at the default rate when the screen's rate is unknown */
    uint64_t reported_millihertz = pres->refresh_millihertz();
    uint64_t compose_millihertz = reported_millihertz != 0 ? reported_millihertz : DEFAULT_REFRESH_MILLIHERTZ;
    const uint64_t compose_interval_ns = refresh_interval_ns(compose_millihertz);
    uint64_t last_compose_ns = 0;

    if (reported_millihertz != 0) {
        printf("stlxdm: serving %ux%u at %llu.%03llu Hz\r\n", pres->width(), pres->height(),
               static_cast<unsigned long long>(reported_millihertz / MILLIHERTZ_PER_HERTZ),
               static_cast<unsigned long long>(reported_millihertz % MILLIHERTZ_PER_HERTZ));
    } else {
        printf("stlxdm: serving %ux%u, refresh rate unknown\r\n", pres->width(), pres->height());
    }

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

        int end_fd = pres->end_fd();
        size_t end_slot = fds.size();
        if (end_fd >= 0) {
            fds.push_back({ end_fd, POLLIN, 0 });
        }

        /* Key repeat and, with damage pending and no present waiting,
         * the compose deadline cap the wait. Idle blocks forever. */
        uint64_t now = now_ns();
        int64_t next_ns = inp.repeat_timeout_ns(now);

        /* The clock bounds every sleep at the next second rollover,
         * except behind the overlay where the bar is frozen */
        int64_t clock_ns = srv.clock_timeout_ns(now);
        if (clock_ns >= 0 && (next_ns < 0 || clock_ns < next_ns)) {
            next_ns = clock_ns;
        }

        /* The latency and missed frames readouts refresh on their own
         * cadence and ask for nothing once quiet */
        int64_t perf_ns = srv.perf_timeout_ns(now);
        if (perf_ns >= 0 && (next_ns < 0 || perf_ns < next_ns)) {
            next_ns = perf_ns;
        }

        if (srv.compose_pending() && end_fd < 0) {
            uint64_t deadline = last_compose_ns + compose_interval_ns;
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
            load_config(config);
            srv.reload_config();
            inp.set_repeat_rates(
                static_cast<uint64_t>(config.key_repeat_delay_ms) * 1000000ull,
                static_cast<uint64_t>(config.key_repeat_interval_ms) * 1000000ull);
        }

        if (rc < 0) {
            continue;
        }

        /* One timestamp serves the repeat timer and marks the arrival
         * of any input this wakeup delivered */
        now = now_ns();
        srv.perf_wake(now);

        if (inp.kbd_fd() >= 0 && (fds[kbd_slot].revents & POLLIN)) {
            inp.pump_kbd(srv);
        }
        if (inp.mouse_fd() >= 0 && (fds[mouse_slot].revents & POLLIN)) {
            inp.pump_mouse(srv);
        }
        inp.pump_repeat(srv, now);
        srv.perf_settle();
        srv.clock_tick();

        srv.pump(fds);

        if (end_fd >= 0 && (fds[end_slot].revents & POLLIN)) {
            pres->take_end();
        }

        now = now_ns();
        srv.perf_tick(now);

        bool present_waiting = pres->end_fd() >= 0;
        bool period_passed = now >= last_compose_ns + compose_interval_ns;
        if (srv.compose_pending() && !present_waiting && period_passed) {
            srv.compose_tick();
            last_compose_ns = now;
        }

        /* Everything this wakeup staged leaves before the next sleep */
        srv.flush_events();
    }
}
