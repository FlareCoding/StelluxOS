#include "display.hpp"
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

/* The rate stlxdm composes an output at when its rate is unknown */
constexpr uint64_t DEFAULT_REFRESH_MILLIHERTZ = 60000;

/* How long stlxdm waits before asking again for display buffers while
 * another program controls the display's buffers */
constexpr long DISPLAY_BUSY_RETRY_NS = 100000000;

/* One output's pace, all zero until it is first paced and while it is not served */
struct output_pace {
    uint64_t compose_interval_ns = 0;
    uint64_t last_compose_ns = 0;
    uint64_t last_cursor_move_ns = 0;
};

/* The pacing state the main loop keeps across wakeups */
struct pacing {
    output_pace outputs[gfxfb::GFXFB_OUTPUTS_MAX];
    bool        showing = true;  /* an output was served at the last pacing, true before the first */
};

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

/* Opens `disp`, waiting while another program controls the display's buffers since a frame drawn into the framebuffer
 * would not show, and returns whether it opened */
static bool open_display(display& disp) {
    bool reported_busy = false;
    while (true) {
        int rc = disp.init();
        if (rc == 0) {
            break;
        }

        if (rc != -EBUSY) {
            return false;
        }

        if (!reported_busy) {
            printf("stlxdm: waiting for the program controlling the display's buffers to close /dev/gfxfb\r\n");
            reported_busy = true;
        }

        timespec retry = { 0, DISPLAY_BUSY_RETRY_NS };
        nanosleep(&retry, nullptr);
    }

    printf("stlxdm: %s\r\n", disp.falls_back() ? "drawing into the framebuffer" : "presenting through display buffers");

    return true;
}

/* Prints an output's size and rate and returns the time between its compositions: a refresh at its rate, or at the
 * default rate when it is unknown */
static uint64_t start_serving(const presenter& pres) {
    uint64_t reported_millihertz = pres.refresh_millihertz();
    if (reported_millihertz != 0) {
        printf("stlxdm: serving %ux%u at %llu.%03llu Hz on output %u\r\n", pres.width(), pres.height(),
               static_cast<unsigned long long>(reported_millihertz / MILLIHERTZ_PER_HERTZ),
               static_cast<unsigned long long>(reported_millihertz % MILLIHERTZ_PER_HERTZ), pres.output());
    } else {
        printf("stlxdm: serving %ux%u, refresh rate unknown, on output %u\r\n", pres.width(), pres.height(),
               pres.output());
    }

    uint64_t compose_millihertz = reported_millihertz != 0 ? reported_millihertz : DEFAULT_REFRESH_MILLIHERTZ;

    return refresh_interval_ns(compose_millihertz);
}

/* Paces each served output from its presenter's refresh when the `outputs` bits name it or nothing paces it yet,
 * printing each, and says once when no output is served */
static void pace_outputs(const display& disp, uint32_t outputs, pacing& pace) {
    bool showing = false;
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        const presenter* pres = disp.presenter_of(output);
        output_pace& out = pace.outputs[output];
        if (!pres) {
            out = {};
            continue;
        }

        bool named = (outputs & (1u << output)) != 0;
        if (named || out.compose_interval_ns == 0) {
            out.compose_interval_ns = start_serving(*pres);
        }

        showing = true;
    }

    if (!showing && pace.showing) {
        printf("stlxdm: drawing nothing until the display changes an output\r\n");
    }

    pace.showing = showing;
}

/* Reads every output that output events named since the last call and has `disp` follow it, an output whose read
 * fails counting as dark, then has the server follow the outputs and moves the pointer where the server put it */
static void follow_outputs(display& disp, server& srv, input& inp, pacing& pace) {
    uint32_t changed = disp.take_changed_outputs();
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        if ((changed & (1u << output)) == 0) {
            continue;
        }

        gfxfb::gfxfb_output_info info;
        if (disp.read_output(output, info) != 0) {
            info = {};
            info.output = output;
        }

        disp.follow_output(output, info);
    }

    srv.outputs_changed(changed);
    inp.set_pointer(srv.follow_outputs(inp.pointer(), inp.buttons()));
    pace_outputs(disp, changed, pace);
}

/* Makes the waiting mode requests one at a time, following their output events before each answer and the outputs
 * again after it, since `keeps_place_for_mode_request()` can turn false at an answer */
static void run_mode_requests(display& disp, server& srv, input& inp, pacing& pace) {
    while (srv.start_mode_request()) {
        if (disp.output_changed()) {
            follow_outputs(disp, srv, inp, pace);
        }

        srv.finish_mode_request();
        inp.set_pointer(srv.follow_outputs(inp.pointer(), inp.buttons()));
    }
}

/* Lowers `next_ns`, -1 while nothing bounds the wait, to `bound_ns` when that comes sooner */
static void bound_wait(int64_t& next_ns, int64_t bound_ns) {
    if (bound_ns >= 0 && (next_ns < 0 || bound_ns < next_ns)) {
        next_ns = bound_ns;
    }
}

static int64_t until(uint64_t deadline, uint64_t now) {
    return deadline > now ? static_cast<int64_t>(deadline - now) : 0;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    display disp;
    if (!open_display(disp)) {
        printf("stlxdm: no framebuffer, exiting\r\n");
        return 1;
    }

    static stlxconf_t config;
    load_config(config);

    if (decor::init() != 0) {
        printf("stlxdm: chrome font unavailable\r\n");
        return 1;
    }

    /* The desktop's state builds first, so the wallpaper decode and the
     * font opens are already paid when the splash asks for Enter */
    server srv;
    if (srv.init(&disp, &config) != 0) {
        printf("stlxdm: desktop init failed\r\n");
        disp.shutdown();
        return 1;
    }

    /* The splash owns the main output until Enter, before any client can
     * connect and before autostart spawns */
    splash_run(*srv.main_presenter());
    srv.switch_to_gpu();

    if (srv.serve() != 0) {
        printf("stlxdm: failed to bind %s\r\n", SWP_SOCKET_PATH);
        disp.shutdown();
        return 1;
    }

    input inp;
    if (inp.init(&srv.desktop(),
                 static_cast<uint64_t>(config.key_repeat_delay_ms) * 1000000ull,
                 static_cast<uint64_t>(config.key_repeat_interval_ms) * 1000000ull) != 0) {
        printf("stlxdm: no input devices, serving without input\r\n");
    }

    signal(SIGHUP, on_sighup);
    write_pidfile();

    /* stlxdm composes each output after its last present ended, at most once per its refresh, at the default rate when
     * its rate is unknown, and sends its waiting cursor move at most once per refresh, even while a present waits */
    pacing pace;

    /* An output event the splash's presents read is followed before the outputs are first paced */
    if (disp.output_changed()) {
        follow_outputs(disp, srv, inp, pace);
    } else {
        pace_outputs(disp, ~0u, pace);
        srv.show_cursor();
    }

    std::vector<pollfd> fds;
    while (true) {
        srv.reap_retired_compositor();

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

        size_t event_slot = fds.size();
        fds.push_back({ disp.fd(), POLLIN, 0 });

        size_t files_slot = fds.size();
        int files_fd = srv.quick_files_fd();
        if (files_fd >= 0) {
            fds.push_back({ files_fd, POLLIN, 0 });
        }

        size_t clipboard_slot = fds.size();
        int clipboard_fd = srv.quick_clipboard_fd();
        if (clipboard_fd >= 0) {
            fds.push_back({ clipboard_fd, POLLIN, 0 });
        }

        size_t images_slot = fds.size();
        int images_fd = srv.quick_images_fd();
        if (images_fd >= 0) {
            fds.push_back({ images_fd, POLLIN, 0 });
        }

        size_t capture_slot = fds.size();
        int capture_fd = srv.capture_fd();
        if (capture_fd >= 0) {
            fds.push_back({ capture_fd, POLLIN, 0 });
        }

        /* Key repeat, the clock, a trial, a drop's finish, a thumbnail's time, a GPU commit's query, a retired
         * compositor's next timeline read and each output's compose and cursor move deadlines cap the wait */
        uint64_t now = now_ns();
        int64_t next_ns = inp.repeat_timeout_ns(now);

        bound_wait(next_ns, srv.clock_timeout_ns(now));
        bound_wait(next_ns, srv.mode_timeout_ns(now));
        bound_wait(next_ns, srv.drag_timeout_ns(now));
        bound_wait(next_ns, srv.thumbnail_timeout_ns(now));
        bound_wait(next_ns, srv.latch_timeout_ns(now));
        bound_wait(next_ns, srv.retired_timeout_ns());

        srv.spread_damage();
        for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
            const presenter* pres = disp.presenter_of(output);
            if (!pres) {
                continue;
            }

            const output_pace& out = pace.outputs[output];
            if (srv.compose_pending(output) && !pres->present_pending()) {
                bound_wait(next_ns, until(out.last_compose_ns + out.compose_interval_ns, now));
            }

            if (srv.cursor_move_pending(output)) {
                bound_wait(next_ns, until(out.last_cursor_move_ns + out.compose_interval_ns, now));
            }
        }

        /* While the main output is not served, a power overlay phase or hold in flight bounds the sleep at a refresh
         * of the default rate, since no compose advances the overlay then */
        bool main_served = srv.main_presenter() != nullptr;
        if (!main_served && srv.power_animating()) {
            bound_wait(next_ns, static_cast<int64_t>(refresh_interval_ns(DEFAULT_REFRESH_MILLIHERTZ)));
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

        now = now_ns();

        if (inp.kbd_fd() >= 0 && (fds[kbd_slot].revents & POLLIN)) {
            inp.pump_kbd(srv);
        }
        if (inp.mouse_fd() >= 0 && (fds[mouse_slot].revents & POLLIN)) {
            inp.pump_mouse(srv);
        }
        inp.pump_repeat(srv, now);
        srv.clock_tick();

        if (files_fd >= 0 && (fds[files_slot].revents & POLLIN)) {
            srv.pump_quick_files();
        }

        if (clipboard_fd >= 0 && (fds[clipboard_slot].revents & POLLIN)) {
            srv.pump_quick_clipboard();
        }

        if (images_fd >= 0 && (fds[images_slot].revents & POLLIN)) {
            srv.pump_quick_images();
        }

        if (capture_fd >= 0 && (fds[capture_slot].revents & POLLIN)) {
            srv.pump_captures();
        }

        srv.pump(fds);
        inp.set_pointer(srv.apply_layout_requests(inp.pointer(), inp.buttons()));

        if (fds[event_slot].revents & POLLIN) {
            disp.take_events();
        }

        if (disp.output_changed()) {
            follow_outputs(disp, srv, inp, pace);
        }

        srv.mode_tick(now);
        srv.drag_tick(now);
        srv.thumbnail_tick(now);
        run_mode_requests(disp, srv, inp, pace);
        srv.sync_shell();

        now = now_ns();

        for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
            output_pace& out = pace.outputs[output];
            bool move_due = now >= out.last_cursor_move_ns + out.compose_interval_ns;
            if (disp.presenter_of(output) && srv.cursor_move_pending(output) && move_due) {
                srv.flush_cursor(output);
                out.last_cursor_move_ns = now;
            }
        }

        srv.spread_damage();
        for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
            const presenter* pres = disp.presenter_of(output);
            output_pace& out = pace.outputs[output];
            bool period_passed = now >= out.last_compose_ns + out.compose_interval_ns;
            if (pres && !pres->present_pending() && period_passed && srv.compose_pending(output)) {
                srv.compose_tick(output);
                out.last_compose_ns = now;
            }
        }

        /* Nothing composes the main output while it is not served, so the
         * power overlay ticks here and a confirmed action still runs */
        if (!srv.main_presenter()) {
            srv.power_tick();
        }

        /* Everything this wakeup staged leaves before the next sleep */
        srv.flush_events();
        srv.flush_outputs();
    }
}
