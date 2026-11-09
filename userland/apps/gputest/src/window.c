// The window checks, each on a connection of its own, with pictures in mappable system memory whose acquire points
// the CPU or a GPU fill reaches. They speak through libstlxwin except to break a rule the library keeps or cannot check

#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stlx/gpu_abi.h>
#include <stlx/proc.h>
#include <stlxwin/stlxwin.h>

#include "gputest.h"

#define PICTURE_PIXELS 256u
#define PICTURE_PITCH  (PICTURE_PIXELS * 4u)
#define THREE_TICKS_NS (50 * 1000000ULL)
#define EVENT_WAIT_NS  (2 * 1000000000ULL)
#define DISPATCH_NS    (10 * 1000000ULL)
#define DRAG_WAIT_NS   (120 * 1000000000ULL)
#define CHECK_SLICE_US 10000u
#define RED            0x00E04040u
#define GREEN          0x0040C040u
#define BLUE           0x004060E0u

// A square picture of this side holds BUFFER_BYTES, the bytes the copy engine's fill writes
#define FILLED_PIXELS 512u
_Static_assert(FILLED_PIXELS * FILLED_PIXELS * 4u == BUFFER_BYTES, "the fill writes exactly the picture's bytes");

// Only the picture on screen waits for a tick to be released, so with more pictures the render thread waits less
#define RENDER_FRAMES   10000u
#define RENDER_PICTURES 12u

// A round trip every ATTACH_SYNC_EVERY cycles bounds gputest's handles in flight at 600, under any limit it inherits
#define ATTACH_SYNC_EVERY 200u

#define READY_SLOT   3
#define GPUTEST_PATH "/bin/gputest"

typedef struct {
    int32_t   buffer;   // with both rights
    int32_t   acquire;  // its point is reached once the picture is drawn
    int32_t   release;  // stlxdm signals it once it no longer reads the picture
    uint32_t  id;       // stlxwin_gpu_attach's, 0 until attached
    uint32_t  width;
    uint32_t  height;
    uint32_t* pixels;
} picture;

typedef struct {
    stlxwin_conn*   conn;
    stlxwin_window* win;
} session;

typedef struct {
    int             gpu;
    stlxwin_window* win;
    picture         pictures[RENDER_PICTURES];
    uint32_t        committed;
    atomic_bool     done;
} render_job;

typedef enum {
    BREAK_BUFFER_RIGHTS,  // a buffer without GPU_RIGHT_READ
    BREAK_BUFFER_TYPE,    // a timeline in the buffer's place
    BREAK_BUFFER_SIZE,    // a picture taller than its buffer
    BREAK_BUFFER_WRAP,    // an offset past the buffer's end whose sum with the picture's bytes wraps to fit it
    BREAK_RELEASE_ORDER,  // a release point that does not rise
    BREAK_HELD_COMMIT,    // a commit of the buffer on screen
    BREAK_RELEASE_RIGHTS, // a release timeline without GPU_RIGHT_WRITE, found at its first release
} rule_break;

static const picture NO_PICTURE = { -1, -1, -1, 0, 0, 0, NULL };
static const session NO_SESSION = { NULL, NULL };

static int32_t create_timeline(int gpu) {
    struct gpu_timeline_args args = {0};
    return ioctl(gpu, GPU_TIMELINE_CREATE, &args) == 0 ? args.timeline : -1;
}

static bool signal_point(int gpu, int32_t timeline, uint64_t value) {
    struct gpu_timeline_args args = {0};
    args.timeline = timeline;
    args.value = value;

    return ioctl(gpu, GPU_TIMELINE_SIGNAL, &args) == 0;
}

static uint64_t timeline_value(int gpu, int32_t timeline) {
    struct gpu_timeline_args args = {0};
    args.timeline = timeline;

    return ioctl(gpu, GPU_TIMELINE_QUERY, &args) == 0 ? args.value : 0;
}

static bool reaches(int gpu, int32_t timeline, uint64_t value, uint64_t timeout_ns) {
    struct gpu_point point = { timeline, 0, value };
    struct gpu_wait_args args = {0};
    args.points = (uint64_t)(uintptr_t)&point;
    args.deadline_ns = monotonic_ns() + timeout_ns;
    args.count = 1;

    return ioctl(gpu, GPU_TIMELINE_WAIT, &args) == 0;
}

static size_t picture_bytes(const picture* pic) {
    return (size_t)pic->width * pic->height * sizeof(uint32_t);
}

static bool make_sized_picture(int gpu, uint32_t color, uint32_t width, uint32_t height, picture* out) {
    out->width = width;
    out->height = height;
    out->acquire = create_timeline(gpu);
    out->release = create_timeline(gpu);

    struct gpu_buffer_args args = {0};
    args.size = picture_bytes(out);
    args.placement = GPU_PLACEMENT_SYSTEM;
    args.flags = GPU_BUFFER_MAPPABLE;
    out->buffer = ioctl(gpu, GPU_BUFFER_CREATE, &args) == 0 ? args.buffer : -1;

    void* pixels = MAP_FAILED;
    if (out->buffer >= 0) {
        pixels = mmap(NULL, picture_bytes(out), PROT_READ | PROT_WRITE, MAP_SHARED, out->buffer, 0);
    }

    if (pixels == MAP_FAILED || out->acquire < 0 || out->release < 0) {
        return false;
    }

    out->pixels = pixels;
    for (size_t i = 0; i < picture_bytes(out) / sizeof(uint32_t); i++) {
        out->pixels[i] = color;
    }

    return true;
}

static bool make_picture(int gpu, uint32_t color, picture* out) {
    return make_sized_picture(gpu, color, PICTURE_PIXELS, PICTURE_PIXELS, out);
}

static void free_picture(picture* pic) {
    if (pic->pixels) {
        munmap(pic->pixels, picture_bytes(pic));
    }

    int32_t handles[] = { pic->buffer, pic->acquire, pic->release };
    for (uint32_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
        if (handles[i] >= 0) {
            close(handles[i]);
        }
    }

    *pic = NO_PICTURE;
}

static bool attach(stlxwin_window* win, picture* pic) {
    stlxwin_gpu_buffer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.buffer = pic->buffer;
    desc.acquire = pic->acquire;
    desc.release = pic->release;

    desc.width = pic->width;
    desc.height = pic->height;
    desc.pitch = pic->width * (uint32_t)sizeof(uint32_t);
    desc.format = STLXWIN_FMT_XRGB8888;

    pic->id = stlxwin_gpu_attach(win, &desc);

    return pic->id != 0;
}

static bool commit(stlxwin_window* win, const picture* pic, uint64_t acquire, uint64_t latch, uint64_t release) {
    return stlxwin_gpu_commit(win, pic->id, acquire, latch, release, NULL, 0, STLXWIN_COMMIT_WANT_FRAME) == 0;
}

// Dispatches until `win` gets an event of `type` or `timeout_ns` passes, counting other windows' in `others`
static bool event_arrives(
    stlxwin_conn* conn,
    stlxwin_window* win,
    stlxwin_event_type type,
    uint64_t timeout_ns,
    uint32_t* others
) {
    uint64_t deadline = monotonic_ns() + timeout_ns;

    while (1) {
        stlxwin_event event;
        while (stlxwin_next_event(conn, &event)) {
            bool is_type = event.type == type;
            if (is_type && event.window == win) {
                return true;
            }

            if (is_type && others) {
                (*others)++;
            }
        }

        uint64_t now = monotonic_ns();
        if (now >= deadline || stlxwin_wait(conn, (int64_t)(deadline - now)) < 0) {
            return false;
        }
    }
}

static bool frame_arrives(stlxwin_conn* conn, stlxwin_window* win, uint64_t timeout_ns, uint32_t* others) {
    return event_arrives(conn, win, STLXWIN_EVT_FRAME, timeout_ns, others);
}

// Shows a drawn picture: reaches its acquire point, commits it and waits for its FRAME_DONE
static bool show(int gpu, session* s, stlxwin_window* win, picture* pic, uint64_t point) {
    bool committed = signal_point(gpu, pic->acquire, point) && commit(win, pic, point, 0, point);
    return committed && frame_arrives(s->conn, win, EVENT_WAIT_NS, NULL);
}

static bool open_sized_session(session* s, uint32_t side, uint32_t window_flags) {
    s->conn = stlxwin_connect("gputest");
    if (s->conn) {
        s->win = stlxwin_window_create(s->conn, side, side, "gputest", window_flags);
    }

    return s->win != NULL;
}

static bool open_session(session* s, uint32_t window_flags) {
    return open_sized_session(s, PICTURE_PIXELS, window_flags);
}

// The library raises every release point it holds as it disconnects
static void close_session(session* s) {
    if (s->conn) {
        stlxwin_disconnect(s->conn);
    }

    *s = NO_SESSION;
}

static void check_latch_waits_for_acquire(int gpu) {
    session s = NO_SESSION;
    picture pic = NO_PICTURE;
    bool ready = open_session(&s, 0) && make_picture(gpu, RED, &pic) && attach(s.win, &pic);
    bool committed = ready && commit(s.win, &pic, 1, 2, 3);

    bool framed_early = committed && frame_arrives(s.conn, s.win, THREE_TICKS_NS, NULL);
    bool latched_early = timeline_value(gpu, pic.release) != 0;
    bool signaled = committed && signal_point(gpu, pic.acquire, 1);
    bool framed = signaled && frame_arrives(s.conn, s.win, EVENT_WAIT_NS, NULL);
    bool latched = timeline_value(gpu, pic.release) == 2;
    check(
        "a GPU picture latches only once its acquire point is reached, its latch point reached by its FRAME_DONE",
        framed && !framed_early && !latched_early && latched
    );

    close_session(&s);
    free_picture(&pic);
}

// The fill waits for a point gputest reaches once it has looked for a latch, so an early latch fails the check
static void check_latch_waits_for_fill(int gpu, const struct gpu_info* info) {
    session s = NO_SESSION;
    picture pic = NO_PICTURE;
    int32_t fill_start = create_timeline(gpu);
    bool opened = fill_start >= 0 && open_sized_session(&s, FILLED_PIXELS, 0);
    bool ready = opened && make_sized_picture(gpu, RED, FILLED_PIXELS, FILLED_PIXELS, &pic) && attach(s.win, &pic);

    struct gpu_point start_point = { fill_start, 0, 1 };
    struct gpu_point acquire_point = { pic.acquire, 0, 1 };
    int32_t fill_context = ready ? submit_fill(gpu, info, pic.buffer, GREEN, &start_point, &acquire_point) : -1;
    bool committed = fill_context >= 0 && commit(s.win, &pic, 1, 2, 3);

    bool framed_early = committed && frame_arrives(s.conn, s.win, THREE_TICKS_NS, NULL);
    bool latched_early = timeline_value(gpu, pic.release) != 0;
    bool unfilled = committed && reads_as(pic.pixels, RED);

    bool started = unfilled && signal_point(gpu, fill_start, 1);
    bool framed = started && frame_arrives(s.conn, s.win, EVENT_WAIT_NS, NULL);
    bool latched = timeline_value(gpu, pic.release) == 2;
    bool filled = framed && reads_as(pic.pixels, GREEN);
    check(
        "a GPU picture latches only once its acquire point is reached by a copy engine's fill of it, "
        "its latch point reached by its FRAME_DONE",
        framed && !framed_early && !latched_early && latched && filled
    );

    close_session(&s);
    free_picture(&pic);
    if (fill_context >= 0) {
        close(fill_context);
    }

    if (fill_start >= 0) {
        close(fill_start);
    }
}

static void check_next_picture_releases(int gpu) {
    session s = NO_SESSION;
    picture first = NO_PICTURE;
    picture second = NO_PICTURE;
    bool ready = open_session(&s, 0) && make_picture(gpu, RED, &first) && make_picture(gpu, GREEN, &second);
    bool attached = ready && attach(s.win, &first) && attach(s.win, &second);

    bool first_shown = attached && show(gpu, &s, s.win, &first, 1);
    bool second_shown = first_shown && show(gpu, &s, s.win, &second, 1);
    check(
        "a second GPU picture's FRAME_DONE comes with the first's release point reached",
        second_shown && timeline_value(gpu, first.release) == 1
    );

    close_session(&s);
    free_picture(&first);
    free_picture(&second);
}

static void check_replaced_picture_releases(int gpu) {
    session s = NO_SESSION;
    picture first = NO_PICTURE;
    picture second = NO_PICTURE;
    bool ready = open_session(&s, 0) && make_picture(gpu, RED, &first) && make_picture(gpu, GREEN, &second);
    bool attached = ready && attach(s.win, &first) && attach(s.win, &second);

    // The first picture's acquire point is never reached, so it is still pending when the second replaces it, and the
    // second's is reached only after the wait, so a FRAME_DONE before then is the first's
    bool committed = attached && commit(s.win, &first, 1, 0, 1);
    bool replaced = committed && commit(s.win, &second, 1, 0, 1);

    bool released = replaced && reaches(gpu, first.release, 1, EVENT_WAIT_NS);
    bool framed_early = released && frame_arrives(s.conn, s.win, THREE_TICKS_NS, NULL);
    bool signaled = released && signal_point(gpu, second.acquire, 1);
    bool framed = signaled && frame_arrives(s.conn, s.win, EVENT_WAIT_NS, NULL);
    check(
        "a GPU picture replaced while pending is released at once with no FRAME_DONE of its own",
        released && !framed_early && framed
    );

    close_session(&s);
    free_picture(&first);
    free_picture(&second);
}

static void check_unsignaled_picture_waits(int gpu) {
    session s = NO_SESSION;
    picture shown = NO_PICTURE;
    picture undrawn = NO_PICTURE;
    picture other = NO_PICTURE;
    bool ready = open_session(&s, 0) && make_picture(gpu, RED, &shown) && make_picture(gpu, GREEN, &undrawn) &&
                 make_picture(gpu, BLUE, &other);

    stlxwin_window* second = NULL;
    if (ready) {
        second = stlxwin_window_create(s.conn, PICTURE_PIXELS, PICTURE_PIXELS, "gputest", 0);
    }

    bool attached = second && attach(s.win, &shown) && attach(s.win, &undrawn) && attach(second, &other);
    bool committed = attached && show(gpu, &s, s.win, &shown, 1) && commit(s.win, &undrawn, 1, 0, 1);

    uint32_t stray_frames = 0;
    bool other_committed = committed && signal_point(gpu, other.acquire, 1) && commit(second, &other, 1, 0, 1);
    bool other_framed = other_committed && frame_arrives(s.conn, second, EVENT_WAIT_NS, &stray_frames);
    bool waited = other_framed && !frame_arrives(s.conn, s.win, THREE_TICKS_NS, NULL);
    check(
        "a window keeps its picture through an acquire point never reached while another window latches",
        waited && stray_frames == 0 && timeline_value(gpu, shown.release) == 0
    );

    close_session(&s);
    free_picture(&shown);
    free_picture(&undrawn);
    free_picture(&other);
}

static void check_detached_picture_stays(int gpu) {
    session s = NO_SESSION;
    picture first = NO_PICTURE;
    picture second = NO_PICTURE;
    picture fill[STLXWIN_GPU_BUFFERS_MAX - 1];
    for (uint32_t i = 0; i < STLXWIN_GPU_BUFFERS_MAX - 1; i++) {
        fill[i] = NO_PICTURE;
    }

    bool ready = open_session(&s, 0) && make_picture(gpu, RED, &first) && make_picture(gpu, GREEN, &second);
    bool attached = ready && attach(s.win, &first) && attach(s.win, &second);

    bool detached = attached && show(gpu, &s, s.win, &first, 1) && stlxwin_gpu_detach(s.win, first.id) == 0;
    bool kept = detached && !reaches(gpu, first.release, 1, THREE_TICKS_NS);

    // The detached picture still counts, so the last of the fill finds the connection full
    bool filled = kept;
    for (uint32_t i = 0; filled && i < STLXWIN_GPU_BUFFERS_MAX - 2; i++) {
        filled = make_picture(gpu, BLUE, &fill[i]) && attach(s.win, &fill[i]);
    }

    picture* last = &fill[STLXWIN_GPU_BUFFERS_MAX - 2];
    bool refused = filled && make_picture(gpu, BLUE, last) && !attach(s.win, last) && stlxwin_gpu_check(s.conn) == 0;
    bool replaced = refused && show(gpu, &s, s.win, &second, 1);
    bool reattached = replaced && timeline_value(gpu, first.release) == 1 && attach(s.win, last) &&
                      show(gpu, &s, s.win, last, 1);
    check(
        "a shown GPU picture detached stays on screen and counts toward the limit until the next picture latches",
        reattached
    );

    close_session(&s);
    free_picture(&first);
    free_picture(&second);
    for (uint32_t i = 0; i < STLXWIN_GPU_BUFFERS_MAX - 1; i++) {
        free_picture(&fill[i]);
    }
}

static int32_t narrowed(int gpu, int32_t handle, uint32_t rights) {
    struct gpu_restrict_args args = {0};
    args.handle = handle;
    args.rights = rights;

    return ioctl(gpu, GPU_HANDLE_RESTRICT, &args) == 0 ? args.restricted : -1;
}

// Attaches `pic` to the raw window as `buf_id` with the rights stlxdm asks of each handle, but for what `rule` breaks
static bool raw_attach(int gpu, int sock, const picture* pic, uint32_t buf_id, rule_break rule) {
    swp_attach_gpu_buffer attach;
    memset(&attach, 0, sizeof(attach));
    attach.win_id = PROTO_WINDOW_ID;
    attach.buf_id = buf_id;

    attach.width = PICTURE_PIXELS;
    attach.height = rule == BREAK_BUFFER_SIZE ? PICTURE_PIXELS + 1 : PICTURE_PIXELS;
    attach.pitch = PICTURE_PITCH;
    attach.format = SWP_FMT_XRGB8888;
    attach.offset = rule == BREAK_BUFFER_WRAP ? UINT64_MAX - 3 : 0;

    int32_t in_buffer = rule == BREAK_BUFFER_TYPE ? pic->acquire : pic->buffer;
    uint32_t buffer_rights = rule == BREAK_BUFFER_RIGHTS ? GPU_RIGHT_WRITE : GPU_RIGHT_READ;
    uint32_t release_rights = rule == BREAK_RELEASE_RIGHTS ? GPU_RIGHT_READ : GPU_RIGHT_WRITE;
    int32_t handles[SWP_GPU_HANDLES] = {
        narrowed(gpu, in_buffer, buffer_rights),
        narrowed(gpu, pic->acquire, GPU_RIGHT_READ),
        narrowed(gpu, pic->release, release_rights),
    };

    bool narrowed_all = true;
    for (uint32_t i = 0; i < SWP_GPU_HANDLES; i++) {
        narrowed_all = narrowed_all && handles[i] >= 0;
    }

    bool sent = narrowed_all &&
                proto_send(sock, SWP_MSG_ATTACH_GPU_BUFFER, &attach, sizeof(attach), handles, SWP_GPU_HANDLES);

    for (uint32_t i = 0; i < SWP_GPU_HANDLES; i++) {
        if (handles[i] >= 0) {
            close(handles[i]);
        }
    }

    return sent;
}

static bool raw_commit(int sock, uint32_t buf_id, uint64_t acquire, uint64_t latch, uint64_t release) {
    swp_commit_gpu commit;
    memset(&commit, 0, sizeof(commit));
    commit.win_id = PROTO_WINDOW_ID;
    commit.buf_id = buf_id;

    commit.acquire_point = acquire;
    commit.latch_point = latch;
    commit.release_point = release;

    return proto_send(sock, SWP_MSG_COMMIT_GPU, &commit, sizeof(commit), NULL, 0);
}

// Breaks `rule` on a raw connection, as libstlxwin keeps it or cannot check it, and checks stlxdm disconnects
static void check_rule_break(int gpu, const char* name, rule_break rule) {
    swp_gpu_info gpu_info;
    picture first = NO_PICTURE;
    picture second = NO_PICTURE;
    int sock = proto_connect(&gpu_info);
    bool ready = sock >= 0 && proto_create_window(sock, NULL, 0) && make_picture(gpu, RED, &first) &&
                 make_picture(gpu, GREEN, &second);

    bool sent = ready && raw_attach(gpu, sock, &first, 1, rule);
    switch (rule) {
    case BREAK_BUFFER_RIGHTS:
    case BREAK_BUFFER_TYPE:
    case BREAK_BUFFER_SIZE:
    case BREAK_BUFFER_WRAP:
        break;
    case BREAK_RELEASE_ORDER:
        // The second picture replaces the first while its acquire point is unreached, releasing it at point 1
        sent = sent && raw_attach(gpu, sock, &second, 2, rule) && raw_commit(sock, 1, 1, 0, 1) &&
               signal_point(gpu, second.acquire, 1) && raw_commit(sock, 2, 1, 0, 1) &&
               reaches(gpu, first.release, 1, EVENT_WAIT_NS) && raw_commit(sock, 1, 2, 0, 1);
        break;
    case BREAK_HELD_COMMIT:
        sent = sent && signal_point(gpu, first.acquire, 1) && raw_commit(sock, 1, 1, 1, 2) &&
               reaches(gpu, first.release, 1, EVENT_WAIT_NS) && raw_commit(sock, 1, 2, 0, 3);
        break;
    case BREAK_RELEASE_RIGHTS:
        // Detaching the picture while its acquire point is unreached releases it at once
        sent = sent && raw_commit(sock, 1, 1, 0, 1) && proto_detach(sock, 1);
        break;
    }

    check(name, sent && proto_closes(sock));

    if (sock >= 0) {
        close(sock);
    }

    free_picture(&first);
    free_picture(&second);
}

int run_held_child(void) {
    int gpu = open("/dev/gpu0", O_RDWR);
    session s = NO_SESSION;
    picture shown = NO_PICTURE;
    picture pending = NO_PICTURE;
    bool ready = gpu >= 0 && open_session(&s, 0) && make_picture(gpu, RED, &shown) &&
                 make_picture(gpu, GREEN, &pending) && attach(s.win, &shown) && attach(s.win, &pending);

    bool held = ready && show(gpu, &s, s.win, &shown, 1) && commit(s.win, &pending, 1, 0, 1);
    char byte = 1;
    if (!held || write(READY_SLOT, &byte, 1) != 1) {
        return 1;
    }

    while (1) {
        sleep(60);
    }
}

static bool byte_arrives(int fd, uint64_t timeout_ns) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    char byte = 0;

    return poll(&pfd, 1, (int)(timeout_ns / 1000000)) == 1 && read(fd, &byte, 1) == 1;
}

// Kills a child holding a GPU picture shown and one pending, then checks a new connection's picture still latches
static void check_killed_client(int gpu) {
    int ends[2] = { -1, -1 };
    // The kernel passes the path as argv[0], so the list starts at argv[1]
    const char* argv[] = { HELD_CHILD_ARG, NULL };
    int child = pipe(ends) == 0 ? proc_create(GPUTEST_PATH, argv) : -1;
    bool started = child >= 0 && proc_set_handle(child, READY_SLOT, ends[1]) == 0 && proc_start(child) == 0;
    if (ends[1] >= 0) {
        close(ends[1]);
    }

    bool ready = started && byte_arrives(ends[0], EVENT_WAIT_NS);
    int status = 0;
    if (started) {
        proc_kill(child);
        proc_wait(child, &status);
    } else if (child >= 0) {
        proc_detach(child);
    }

    session s = NO_SESSION;
    picture pic = NO_PICTURE;
    bool composing = ready && open_session(&s, 0) && make_picture(gpu, BLUE, &pic) && attach(s.win, &pic) &&
                     show(gpu, &s, s.win, &pic, 1);
    check("a client killed with a GPU picture shown and one pending leaves stlxdm composing", composing);

    close_session(&s);
    free_picture(&pic);
    if (ends[0] >= 0) {
        close(ends[0]);
    }
}

// Empties the event queue and returns how many DISCONNECTED events it held
static uint32_t drain_events(stlxwin_conn* conn) {
    uint32_t disconnects = 0;
    stlxwin_event event;
    while (stlxwin_next_event(conn, &event)) {
        if (event.type == STLXWIN_EVT_DISCONNECTED) {
            disconnects++;
        }
    }

    return disconnects;
}

static bool gpu_check_finds_end(stlxwin_conn* conn, uint64_t timeout_ns) {
    uint64_t deadline = monotonic_ns() + timeout_ns;
    while (stlxwin_gpu_check(conn) == 0) {
        if (monotonic_ns() >= deadline) {
            return false;
        }

        usleep(CHECK_SLICE_US);
    }

    return true;
}

// No latch follows the show and stlxdm drops a client without reaching its points, so the library reaches both
static void check_ended_connection_releases(int gpu) {
    session s = NO_SESSION;
    picture shown = NO_PICTURE;
    picture pending = NO_PICTURE;
    bool ready = open_session(&s, 0) && make_picture(gpu, RED, &shown) && make_picture(gpu, GREEN, &pending) &&
                 attach(s.win, &shown) && attach(s.win, &pending);
    bool held = ready && show(gpu, &s, s.win, &shown, 1) && commit(s.win, &pending, 1, 0, 1);

    // `PROTO_WINDOW_ID` is the id of the library's first window, so stlxdm finds it taken and ends the connection
    bool broken = held && proto_create_window(stlxwin_conn_fd(s.conn), NULL, 0);
    bool ended = broken && gpu_check_finds_end(s.conn, EVENT_WAIT_NS);
    bool released = ended && timeline_value(gpu, shown.release) == 1 && timeline_value(gpu, pending.release) == 1;

    bool drained = released && drain_events(s.conn) == 0;

    // A wait on an empty queue returns 1 for a dead connection only by dispatching DISCONNECTED
    bool waited = drained && stlxwin_wait(s.conn, 0) == 1;
    bool stays_ended = waited && drain_events(s.conn) == 1 && stlxwin_dispatch(s.conn) == -1 &&
                       stlxwin_wait(s.conn, 0) == -1;
    check(
        "a connection stlxdm ends reaches its shown and pending pictures' release points and queues one DISCONNECTED",
        stays_ended
    );

    close_session(&s);
    free_picture(&shown);
    free_picture(&pending);
}

static void check_disconnect_releases(int gpu) {
    session s = NO_SESSION;
    picture pic = NO_PICTURE;
    bool ready = open_session(&s, 0) && make_picture(gpu, BLUE, &pic) && attach(s.win, &pic);
    bool detached = ready && show(gpu, &s, s.win, &pic, 1) && stlxwin_gpu_detach(s.win, pic.id) == 0;

    close_session(&s);
    check(
        "stlxwin_disconnect reaches the release point of a detached GPU picture stlxdm still shows",
        detached && timeline_value(gpu, pic.release) == 1
    );

    free_picture(&pic);
}

// stlxdm reaches only the destroyed window's points, so a point reached on the other window is the library's
static void check_destroy_keeps_other_window(int gpu) {
    session s = NO_SESSION;
    picture destroyed = NO_PICTURE;
    picture kept = NO_PICTURE;
    bool ready = open_session(&s, 0) && make_picture(gpu, RED, &destroyed) && make_picture(gpu, GREEN, &kept);

    stlxwin_window* other = NULL;
    if (ready) {
        other = stlxwin_window_create(s.conn, PICTURE_PIXELS, PICTURE_PIXELS, "gputest", 0);
    }

    bool attached = other && attach(s.win, &destroyed) && attach(other, &kept);
    bool shown = attached && show(gpu, &s, s.win, &destroyed, 1) && show(gpu, &s, other, &kept, 1);
    if (shown) {
        stlxwin_window_destroy(s.win);
        s.win = NULL;
    }

    bool released = shown && reaches(gpu, destroyed.release, 1, EVENT_WAIT_NS);
    bool held = released && !reaches(gpu, kept.release, 1, THREE_TICKS_NS) && stlxwin_gpu_detach(other, kept.id) == 0;
    check(
        "destroying one of two windows reaches its picture's release point and keeps the other's held and attached",
        held
    );

    close_session(&s);
    free_picture(&destroyed);
    free_picture(&kept);
}

static void check_attach_cycles(int gpu) {
    session s = NO_SESSION;
    picture cycled = NO_PICTURE;
    picture syncs[2] = { NO_PICTURE, NO_PICTURE };
    bool ready = open_session(&s, 0) && make_picture(gpu, RED, &cycled) && make_picture(gpu, GREEN, &syncs[0]) &&
                 make_picture(gpu, BLUE, &syncs[1]) && attach(s.win, &syncs[0]) && attach(s.win, &syncs[1]);

    uint32_t cycles = 0;
    uint32_t synced = 0;
    while (ready && cycles < ATTACH_CYCLES && attach(s.win, &cycled) && stlxwin_gpu_detach(s.win, cycled.id) == 0) {
        cycles++;

        // stlxdm reads in order, so a sync picture's FRAME_DONE shows it took every attach before. Each sync picture's
        // latch releases the other, so a picture's points rise by one per use
        if (cycles % ATTACH_SYNC_EVERY == 0) {
            if (!show(gpu, &s, s.win, &syncs[synced % 2], synced / 2 + 1)) {
                break;
            }

            synced++;
        }
    }

    bool all_taken = cycles == ATTACH_CYCLES && synced == ATTACH_CYCLES / ATTACH_SYNC_EVERY;
    check(
        "66000 GPU buffer attach and detach cycles on a new connection never run stlxdm out of handles",
        all_taken && stlxwin_gpu_check(s.conn) == 0
    );

    close_session(&s);
    free_picture(&cycled);
    free_picture(&syncs[0]);
    free_picture(&syncs[1]);
}

// Commits the job's pictures in turn, each again only once its last release point is reached
static void* render(void* arg) {
    render_job* job = arg;
    uint64_t points[RENDER_PICTURES] = {0};

    for (uint32_t frame = 0; frame < RENDER_FRAMES; frame++) {
        uint32_t index = frame % RENDER_PICTURES;
        picture* pic = &job->pictures[index];
        bool released = points[index] == 0 || reaches(job->gpu, pic->release, points[index], EVENT_WAIT_NS);

        uint64_t point = points[index] + 1;
        bool drawn = released && signal_point(job->gpu, pic->acquire, point);
        if (!drawn || !commit(job->win, pic, point, 0, point)) {
            break;
        }

        points[index] = point;
        job->committed++;
    }

    atomic_store(&job->done, true);

    return NULL;
}

static void check_render_thread(int gpu) {
    session s = NO_SESSION;
    render_job job;
    memset(&job, 0, sizeof(job));
    job.gpu = gpu;
    atomic_init(&job.done, false);

    bool ready = open_session(&s, 0);
    for (uint32_t i = 0; i < RENDER_PICTURES; i++) {
        job.pictures[i] = NO_PICTURE;
        ready = ready && make_picture(gpu, i % 2 ? RED : BLUE, &job.pictures[i]) && attach(s.win, &job.pictures[i]);
    }

    job.win = s.win;
    pthread_t thread;
    bool started = ready && pthread_create(&thread, NULL, render, &job) == 0;

    uint32_t frames = 0;
    while (started && !atomic_load(&job.done)) {
        stlxwin_wait(s.conn, DISPATCH_NS);

        stlxwin_event event;
        while (stlxwin_next_event(s.conn, &event)) {
            if (event.type == STLXWIN_EVT_FRAME) {
                frames++;
            }
        }
    }

    if (started) {
        pthread_join(thread, NULL);
    }

    check(
        "a render thread commits 10000 GPU pictures, each after its release point, while the main thread dispatches",
        job.committed == RENDER_FRAMES && frames > 0 && stlxwin_gpu_check(s.conn) == 0
    );

    close_session(&s);
    for (uint32_t i = 0; i < RENDER_PICTURES; i++) {
        free_picture(&job.pictures[i]);
    }
}

void run_resize_check(int gpu) {
    session s = NO_SESSION;
    picture opened = NO_PICTURE;
    picture late = NO_PICTURE;
    picture resized = NO_PICTURE;
    bool ready = open_session(&s, STLXWIN_WF_RESIZABLE) && make_picture(gpu, RED, &opened) &&
                 make_picture(gpu, GREEN, &late) && attach(s.win, &opened) && attach(s.win, &late);

    bool shown = ready && show(gpu, &s, s.win, &opened, 1);
    if (shown) {
        printf("gputest: a %ux%u window waits for a resize\n", PICTURE_PIXELS, PICTURE_PIXELS);
    }

    uint32_t width = 0;
    uint32_t height = 0;
    bool configured = shown && event_arrives(s.conn, s.win, STLXWIN_EVT_CONFIGURE, DRAG_WAIT_NS, NULL);
    if (configured) {
        stlxwin_window_configured(s.win, &width, &height, NULL);
    }

    bool is_new_size = configured && (width != PICTURE_PIXELS || height != PICTURE_PIXELS);
    bool late_shown = is_new_size && show(gpu, &s, s.win, &late, 1);
    bool made = late_shown && make_sized_picture(gpu, BLUE, width, height, &resized) && attach(s.win, &resized);
    bool resized_shown = made && show(gpu, &s, s.win, &resized, 1);
    check(
        "after a resize's CONFIGURE, a GPU picture at the old size and then one at the new size latch in turn",
        resized_shown && stlxwin_gpu_check(s.conn) == 0
    );

    close_session(&s);
    free_picture(&opened);
    free_picture(&late);
    free_picture(&resized);
}

void run_window_checks(int gpu, const struct gpu_info* info) {
    check_latch_waits_for_acquire(gpu);
    check_next_picture_releases(gpu);
    check_replaced_picture_releases(gpu);
    check_unsignaled_picture_waits(gpu);
    check_detached_picture_stays(gpu);

    if (info->backend == GPU_BACKEND_NVIDIA) {
        check_latch_waits_for_fill(gpu, info);
    }

    check_rule_break(gpu, "an attach of a buffer without GPU_RIGHT_READ disconnects", BREAK_BUFFER_RIGHTS);
    check_rule_break(gpu, "an attach with a timeline in the buffer's place disconnects", BREAK_BUFFER_TYPE);
    check_rule_break(gpu, "an attach of a picture taller than its buffer disconnects", BREAK_BUFFER_SIZE);
    check_rule_break(
        gpu,
        "an attach at an offset past its buffer's end that wraps to fit disconnects",
        BREAK_BUFFER_WRAP
    );

    check_rule_break(gpu, "a commit whose release point does not rise disconnects", BREAK_RELEASE_ORDER);
    check_rule_break(gpu, "a commit of the GPU buffer on screen disconnects", BREAK_HELD_COMMIT);
    check_rule_break(
        gpu,
        "a release timeline without GPU_RIGHT_WRITE disconnects at its first release",
        BREAK_RELEASE_RIGHTS
    );

    check_killed_client(gpu);
    check_ended_connection_releases(gpu);
    check_disconnect_releases(gpu);
    check_destroy_keeps_other_window(gpu);
    check_attach_cycles(gpu);
    check_render_thread(gpu);
}
