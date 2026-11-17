#define _GNU_SOURCE

#include "monitor.h"
#include "listing.h"
#include "port.h"
#include "relay.h"
#include "report.h"
#include "settings.h"
#include "terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RECEIVE_CHUNK_BYTES 4096
#define SHOWN_BUFFER_BYTES  (2 * RECEIVE_CHUNK_BYTES) // what one read becomes when every byte shows as CR LF
#define SEND_BUFFER_BYTES   4096
#define TYPED_CHUNK_BYTES   256
#define REOPEN_MS           500 // how long the wait after a failed reopen lasts before the next one
#define NS_PER_MS           1000000L
#define READABLE            (POLLIN | POLLHUP | POLLERR | POLLNVAL)

// How the session goes on after one of its steps
typedef enum {
    SESSION_GOES_ON,
    SESSION_ENDED,  // by Ctrl-], SIGINT, SIGTERM, SIGHUP or a closed stdout
    SESSION_FAILED, // already reported
} session_turn;

// The steps of an open, in order
typedef enum {
    STEP_OPEN,
    STEP_LINE,
    STEP_LEVELS,
    STEP_PULSE,
    STEP_BREAK,
} open_step;

// The files a turn waits on, one per slot, and a slot whose fd is -1 waits on nothing
typedef enum {
    SLOT_PORT, // the port while it is open, or the listing while the board is gone
    SLOT_STDOUT,
    SLOT_STDIN,
    SLOT_COUNT,
} poll_slot;

typedef struct {
    const serial_args* args;
    serial_port        port;
    relay              relay;
    int                listing_fd;
    tried_arrival      tried;             // the arrival the last open tried
    bool               has_opened;        // an open was reported, so closed follows
    bool               has_send_failed;   // a write failed, so nothing goes to the board until it comes back
    bool               is_dropping;       // typed bytes were dropped since the last ones that fit
    bool               is_stdin_open;
    bool               is_stdin_terminal;
    uint8_t            send[SEND_BUFFER_BYTES];
    size_t             send_length;
    uint8_t            shown[SHOWN_BUFFER_BYTES]; // bytes stdout has yet to take
    size_t             shown_length;
} session;

static const char* const STEP_ACTIONS[] = {
    [STEP_OPEN]   = "open",
    [STEP_LINE]   = "set the line settings of",
    [STEP_LEVELS] = "set DTR and RTS on",
    [STEP_PULSE]  = "pulse a line of",
    [STEP_BREAK]  = "send a break on",
};

// The signals that end a session, then 0 to end the list
static const int ENDING_SIGNALS[] = { SIGINT, SIGTERM, SIGHUP, 0 };

static const struct timespec REOPEN_WAIT = { .tv_sec = 0, .tv_nsec = REOPEN_MS * NS_PER_MS };

static volatile sig_atomic_t g_is_ending;
static sigset_t              g_waiting_mask; // the signal mask while the session waits, the ending signals unblocked

static void note_ending(int signal_number) {
    (void)signal_number;
    g_is_ending = 1;
}

/* The ending signals stay blocked except while the session waits in ppoll, so none comes between the check of the flag
 * and the wait, and with no SA_RESTART the wait returns EINTR */
static void set_up_ending_signals(void) {
    struct sigaction ending = { .sa_handler = note_ending };
    sigset_t ending_signals;
    sigemptyset(&ending.sa_mask);
    sigemptyset(&ending_signals);
    for (const int* signal_number = ENDING_SIGNALS; *signal_number != 0; signal_number++) {
        sigaction(*signal_number, &ending, NULL);
        sigaddset(&ending_signals, *signal_number);
    }

    signal(SIGPIPE, SIG_IGN);
    sigprocmask(SIG_BLOCK, &ending_signals, &g_waiting_mask);
    for (const int* signal_number = ENDING_SIGNALS; *signal_number != 0; signal_number++) {
        sigdelset(&g_waiting_mask, *signal_number);
    }
}

/* A ppoll that finds a file ready returns without delivering, so an ending signal can stay pending through output
 * that never pauses */
static bool is_ending_signal_pending(void) {
    sigset_t pending;
    sigpending(&pending);
    for (const int* signal_number = ENDING_SIGNALS; *signal_number != 0; signal_number++) {
        if (sigismember(&pending, *signal_number)) {
            return true;
        }
    }

    return false;
}

static void report_open_failure(const session* s, open_step step, int error) {
    const char* name = s->port.name;
    failure_kind kind = failure_of_errno(error);
    if (kind == FAILURE_MISSING) {
        report_failure(name, kind, "no serial port is named %s", name);
    } else if (kind == FAILURE_BUSY) {
        report_failure(name, kind, "%s is held by another program", name);
    } else if (kind == FAILURE_SETTINGS && step == STEP_LINE) {
        char described[LINE_DESCRIPTION_MAX];
        line_settings_describe(&s->args->line, described, sizeof(described));
        report_failure(name, kind, "%s refused %s", name, described);
    } else {
        report_failure(name, kind, "cannot %s %s: %s", STEP_ACTIONS[step], name, strerror(error));
    }
}

/* A return whose open fails with ENOENT or EIO is tried again while its arrival stays listed, since the port can leave
 * during its open, and its chip can fail one of the open's requests */
static session_turn close_failed_open(session* s, bool is_return, open_step step, int error) {
    port_close(&s->port);
    if (is_return && (error == ENOENT || error == EIO)) {
        s->tried.is_retrying = true;
        return SESSION_GOES_ON;
    }

    report_open_failure(s, step, error);
    return SESSION_FAILED;
}

static session_turn open_port(session* s, bool is_return) {
    const serial_args* args = s->args;
    if (port_open(&s->port) != 0) {
        return close_failed_open(s, is_return, STEP_OPEN, errno);
    }

    if (port_set_line(&s->port, &args->line) != 0) {
        return close_failed_open(s, is_return, STEP_LINE, errno);
    }

    if (port_set_levels(&s->port, &args->levels) != 0) {
        return close_failed_open(s, is_return, STEP_LEVELS, errno);
    }

    if (!is_return && args->pulse != PULSE_NONE && port_pulse(&s->port, args->pulse) != 0) {
        return close_failed_open(s, is_return, STEP_PULSE, errno);
    }

    if (!is_return && args->break_ms > 0 && port_break(&s->port, args->break_ms) != 0) {
        return close_failed_open(s, is_return, STEP_BREAK, errno);
    }

    if (is_return) {
        relay_restart(&s->relay);
        report_back(s->port.name);
    } else {
        relay_init(&s->relay, &args->session);
        report_open(s->port.name, &args->line);
    }

    s->tried.is_retrying = false;
    s->has_send_failed = false;
    s->is_dropping = false;
    s->has_opened = true;
    return SESSION_GOES_ON;
}

/* Every open follows a listing snapshot, so the wait after a departure ends only at a later arrival of the port, or
 * at a retry of an open that failed */
static int read_offer(session* s, bool* is_offered) {
    listing ports = { 0 };
    if (listing_read(s->listing_fd, &ports) != 0) {
        return -1;
    }

    *is_offered = listing_offers(&ports, s->port.name, &s->tried);
    listing_free(&ports);
    return 0;
}

static void remove_shown_prefix(session* s, size_t count) {
    s->shown_length -= count;
    memmove(s->shown, s->shown + count, s->shown_length);
}

/* Writes what stdout takes of the shown bytes, ending the session when stdout closes. POLLOUT promised room for one
 * byte, so the write returns at once, unless it meets a pty's line end, which takes its CR LF whole */
static session_turn write_shown(session* s) {
    ssize_t count = write(STDOUT_FILENO, s->shown, s->shown_length);
    if (count < 0 && errno == EPIPE) {
        return SESSION_ENDED;
    }

    if (count < 0 && errno != EAGAIN) {
        report_failure("-", FAILURE_IO, "cannot write to stdout: %s", strerror(errno));
        return SESSION_FAILED;
    }

    if (count > 0) {
        remove_shown_prefix(s, (size_t)count);
    }

    return SESSION_GOES_ON;
}

/* At the end stdout gets only what it takes at once, so a stalled reader cannot hold up the end */
static void flush_shown(session* s) {
    struct pollfd stdout_ready = { .fd = STDOUT_FILENO, .events = POLLOUT };
    while (s->shown_length > 0 && poll(&stdout_ready, 1, 0) == 1 && stdout_ready.revents == POLLOUT) {
        ssize_t count = write(STDOUT_FILENO, s->shown, s->shown_length);
        if (count <= 0) {
            return;
        }

        remove_shown_prefix(s, (size_t)count);
    }
}

/* Queues the echo of typed bytes for stdout, dropping what finds no room while its reader stalls */
static void queue_echo(session* s, const uint8_t* echo, size_t length) {
    size_t room = SHOWN_BUFFER_BYTES - s->shown_length;
    size_t queued = length < room ? length : room;
    memcpy(s->shown + s->shown_length, echo, queued);
    s->shown_length += queued;
}

/* A read of 0, or a read that fails, means the port is done, after it handed over every byte it had */
static void receive_bytes(session* s) {
    uint8_t received[RECEIVE_CHUNK_BYTES];
    ssize_t count = read(s->port.fd, received, sizeof(received));
    if (count < 0 && errno == EAGAIN) {
        return;
    }

    if (count <= 0) {
        report_gone(s->port.name, count == 0 ? 0 : errno);
        port_close(&s->port);
        s->send_length = 0;
        return;
    }

    s->shown_length = relay_received(&s->relay, received, (size_t)count, s->shown);
    size_t unread = 0;
    if (s->relay.options.xonxoff && port_count_unread(&s->port, &unread) == 0) {
        relay_watch_queue(&s->relay, unread);
    }
}

/* Writes the pending XON or XOFF, or else the typed bytes the board takes. A failed write stops sending until the
 * board comes back, while reads go on, so its last bytes still show */
static void send_bytes(session* s) {
    uint8_t flow = s->relay.flow_pending;
    bool is_flow = flow != 0;
    if (!is_flow && (s->relay.is_sending_paused || s->send_length == 0)) {
        return;
    }

    ssize_t count = is_flow ? write(s->port.fd, &flow, 1) : write(s->port.fd, s->send, s->send_length);
    if (count < 0 && errno != EAGAIN) {
        s->has_send_failed = true;
        s->send_length = 0;
    } else if (count > 0 && is_flow) {
        relay_flow_sent(&s->relay);
    } else if (count > 0) {
        s->send_length -= (size_t)count;
        memmove(s->send, s->send + count, s->send_length);
    }
}

static void queue_typed_bytes(session* s, const uint8_t* bytes, size_t length) {
    bool can_send = s->port.fd >= 0 && !s->has_send_failed;
    if (can_send && length <= SEND_BUFFER_BYTES - s->send_length) {
        memcpy(s->send + s->send_length, bytes, length);
        s->send_length += length;
        s->is_dropping = false;
        return;
    }

    // While the board is gone, the sentence about its leaving already says why typed bytes are dropped
    if (s->port.fd >= 0 && !s->is_dropping) {
        report_dropping(s->port.name);
        s->is_dropping = true;
    }
}

static session_turn take_typed_bytes(session* s) {
    uint8_t typed[TYPED_CHUNK_BYTES];
    ssize_t count = read(STDIN_FILENO, typed, sizeof(typed));
    if (count <= 0) {
        s->is_stdin_open = false;
        return SESSION_GOES_ON;
    }

    uint8_t to_port[2 * TYPED_CHUNK_BYTES];
    uint8_t to_screen[2 * TYPED_CHUNK_BYTES];
    typed_bytes taken = relay_typed(&s->relay, typed, (size_t)count, to_port, to_screen);
    if (taken.to_port > 0) {
        queue_typed_bytes(s, to_port, taken.to_port);
    }

    queue_echo(s, to_screen, taken.to_screen);
    return taken.is_quit ? SESSION_ENDED : SESSION_GOES_ON;
}

/* A terminal is always read, so Ctrl-] always works, and a pipe or a file only while its bytes can be sent */
static bool wants_typed_bytes(const session* s) {
    bool has_room = SEND_BUFFER_BYTES - s->send_length >= 2 * TYPED_CHUNK_BYTES;
    bool can_take = s->port.fd >= 0 && !s->has_send_failed && has_room;

    return s->is_stdin_open && (s->is_stdin_terminal || can_take);
}

static bool wants_to_send(const session* s) {
    bool has_typed_bytes = s->send_length > 0 && !s->relay.is_sending_paused;
    return !s->has_send_failed && (s->relay.flow_pending != 0 || has_typed_bytes);
}

/* The port is read only once stdout took every shown byte, since a read can show twice its length */
static void serve_port(session* s, short events) {
    if ((events & READABLE) && s->shown_length == 0) {
        receive_bytes(s);
    }

    if (s->port.fd >= 0 && (events & POLLOUT)) {
        send_bytes(s);
    }
}

static session_turn serve_listing(session* s) {
    bool is_offered = false;
    if (read_offer(s, &is_offered) != 0) {
        report_failure("-", FAILURE_IO, "cannot read %s: %s", LISTING_PATH, strerror(errno));
        return SESSION_FAILED;
    }

    return is_offered ? open_port(s, true) : SESSION_GOES_ON;
}

/* Relays bytes both ways while the port is open, and waits in the listing for the board while it is gone. A reader
 * that stops taking stdout stops the reads of the port, and the turns still read Ctrl-] and see the ending signals */
static session_turn run_session(session* s) {
    while (!g_is_ending && !is_ending_signal_pending()) {
        bool is_open = s->port.fd >= 0;
        bool is_reading_stdin = wants_typed_bytes(s);
        short port_events = (short)((s->shown_length == 0 ? POLLIN : 0) | (wants_to_send(s) ? POLLOUT : 0));
        struct pollfd ready[SLOT_COUNT] = {
            [SLOT_PORT]   = { .fd = s->listing_fd, .events = POLLIN },
            [SLOT_STDOUT] = { .fd = s->shown_length > 0 ? STDOUT_FILENO : -1, .events = POLLOUT },
            [SLOT_STDIN]  = { .fd = is_reading_stdin ? STDIN_FILENO : -1, .events = POLLIN },
        };

        // A gone port polls POLLHUP for good, so it waits on nothing while it is neither read nor written
        if (is_open) {
            ready[SLOT_PORT] = (struct pollfd){ .fd = port_events != 0 ? s->port.fd : -1, .events = port_events };
        }

        const struct timespec* timeout = !is_open && s->tried.is_retrying ? &REOPEN_WAIT : NULL;
        int ready_count = ppoll(ready, SLOT_COUNT, timeout, &g_waiting_mask);
        if (ready_count < 0 && errno == EINTR) {
            continue;
        }

        if (ready_count < 0) {
            report_failure("-", FAILURE_IO, "cannot wait for input: %s", strerror(errno));
            return SESSION_FAILED;
        }

        session_turn turn = SESSION_GOES_ON;
        if (ready[SLOT_STDOUT].revents != 0) {
            turn = write_shown(s);
        }

        if (turn == SESSION_GOES_ON && is_open) {
            serve_port(s, ready[SLOT_PORT].revents);
        } else if (turn == SESSION_GOES_ON && (ready_count == 0 || (ready[SLOT_PORT].revents & POLLIN))) {
            turn = serve_listing(s);
        }

        // Serving the port can close it or fail its send, and then a pipe's bytes wait in the pipe
        bool is_typed = ready[SLOT_STDIN].revents & READABLE;
        if (turn == SESSION_GOES_ON && is_typed && wants_typed_bytes(s)) {
            turn = take_typed_bytes(s);
        }

        if (turn != SESSION_GOES_ON) {
            return turn;
        }
    }

    return SESSION_ENDED;
}

static session_turn start_session(session* s) {
    bool is_offered = false;
    s->listing_fd = open(LISTING_PATH, O_RDONLY);
    if (s->listing_fd < 0 || read_offer(s, &is_offered) != 0) {
        report_failure("-", FAILURE_IO, "cannot read %s: %s", LISTING_PATH, strerror(errno));
        return SESSION_FAILED;
    }

    return open_port(s, false);
}

int run_monitor(const serial_args* args) {
    set_up_ending_signals();
    int raw = terminal_enter_raw();
    report_use(stderr, args->has_events, raw == 1);
    if (raw < 0) {
        return report_failure("-", FAILURE_IO, "cannot make the terminal raw: %s", strerror(errno));
    }

    session s = {
        .args              = args,
        .port              = { .fd = -1, .name = args->port },
        .listing_fd        = -1,
        .is_stdin_open     = true,
        .is_stdin_terminal = raw == 1,
    };

    session_turn turn = start_session(&s);
    if (turn == SESSION_GOES_ON) {
        turn = run_session(&s);
    }

    flush_shown(&s);
    port_close(&s.port);
    if (s.has_opened) {
        report_closed(args->port);
    }

    if (s.listing_fd >= 0) {
        close(s.listing_fd);
    }

    terminal_restore();
    return turn == SESSION_FAILED ? 1 : 0;
}
