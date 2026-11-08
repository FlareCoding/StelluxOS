// The window protocol's checks on raw sockets: the handshake's GPU_INFO, stlxdm's handle rules (each check passing
// copies of a pipe's read end and ending in a disconnect that closes them all), then shared memory's attach cycles and
// the bound on one connection's shared memory buffers

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stlxwin/proto.h>

#include "gputest.h"

#define WAIT_NS       (2 * 1000000000ULL)
#define POLL_MS       100
#define HANDLES_MAX   (SWP_GPU_HANDLES + 1)
#define WINDOW_PIXELS 64u
#define READ_CHUNK    512u
#define SHM_DIR       "/dev/shm/"
#define SHM_PATH_MAX  (sizeof(SHM_DIR) + SWP_SHM_NAME_MAX)

// Cycles per round trip, few enough that their 7,200 bytes fit a socket's 8 KiB buffer and no send waits on stlxdm
#define SHM_SYNC_EVERY 60u
_Static_assert(ATTACH_CYCLES % SHM_SYNC_EVERY == 0, "a round trip follows the last cycle");

typedef struct {
    swp_header            header;
    swp_attach_gpu_buffer attach;
} attach_message;

// How a check passes its handles, each way one that stlxdm refuses with a disconnect
typedef enum {
    SEND_ATTACH,         // three with a whole attach to a window made first
    SEND_CREATE_WINDOW,  // one with a whole CREATE_WINDOW
    SEND_FOUR,           // four with an attach's first byte, its rest never sent
    SEND_OTHER_START,    // one with the type of a CREATE_WINDOW, its rest never sent
    SEND_SPLIT_ATTACH,   // three with an attach's first byte, its rest once the connection stayed open 2 seconds
    SEND_THIRD_BATCH,    // three batches of three with an attach's next byte, the third once open 2 seconds
    SEND_SHORT_ATTACH,   // two with a whole attach and the first byte of another
} handle_send;

// Reads `length` bytes, failing when the socket closes or `deadline` passes first
static bool read_by(int sock, void* bytes, size_t length, uint64_t deadline) {
    size_t have = 0;

    while (have < length && monotonic_ns() < deadline) {
        struct pollfd pfd = { sock, POLLIN, 0 };
        if (poll(&pfd, 1, POLL_MS) <= 0) {
            continue;
        }

        ssize_t n = recv(sock, (uint8_t*)bytes + have, length - have, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            return false;
        }

        if (n > 0) {
            have += (size_t)n;
        }
    }

    return have == length;
}

// Reads `length` bytes, failing when the socket closes or 2 seconds pass first
static bool read_whole(int sock, void* bytes, size_t length) {
    return read_by(sock, bytes, length, monotonic_ns() + WAIT_NS);
}

int proto_connect(swp_gpu_info* gpu_info) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SWP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    struct {
        swp_header header;
        swp_hello  hello;
    } hello;
    memset(&hello, 0, sizeof(hello));
    hello.header = (swp_header){ SWP_MSG_HELLO, 0, sizeof(hello.hello) };
    hello.hello.proto_version = SWP_VERSION;
    strncpy(hello.hello.app_id, "gputest", sizeof(hello.hello.app_id) - 1);

    struct {
        swp_header      header;
        swp_hello_reply reply;
        swp_header      info_header;
        swp_gpu_info    info;
    } answer;
    memset(&answer, 0, sizeof(answer));

    bool connected = connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0;
    bool greeted = connected && write(sock, &hello, sizeof(hello)) == (ssize_t)sizeof(hello);
    bool answered = greeted && read_whole(sock, &answer, sizeof(answer));

    bool replied = answer.header.type == SWP_MSG_HELLO_REPLY && answer.header.length == sizeof(answer.reply);
    bool announces_info = (answer.header.flags & SWP_HELLO_GPU_INFO) != 0;
    bool sized_info = answer.info_header.type == SWP_MSG_GPU_INFO && answer.info_header.length == sizeof(answer.info);
    if (!answered || !replied || !announces_info || !sized_info) {
        close(sock);
        return -1;
    }

    *gpu_info = answer.info;

    return sock;
}

// Sends `length` bytes in one sendmsg, with `count` handles in its control data
static bool send_bytes(int sock, const void* bytes, uint32_t length, const int32_t* handles, uint32_t count) {
    if (count > HANDLES_MAX) {
        return false;
    }

    union {
        struct cmsghdr head;
        uint8_t        space[CMSG_SPACE(HANDLES_MAX * sizeof(int32_t))];
    } control;
    memset(&control, 0, sizeof(control));

    struct iovec iov = { (void*)bytes, length };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (count > 0) {
        msg.msg_control = control.space;
        msg.msg_controllen = CMSG_SPACE(count * sizeof(int32_t));

        struct cmsghdr* head = CMSG_FIRSTHDR(&msg);
        head->cmsg_level = SOL_SOCKET;
        head->cmsg_type = SCM_RIGHTS;
        head->cmsg_len = CMSG_LEN(count * sizeof(int32_t));
        memcpy(CMSG_DATA(head), handles, count * sizeof(int32_t));
    }

    return sendmsg(sock, &msg, MSG_NOSIGNAL) == (ssize_t)length;
}

bool proto_send(int sock, uint16_t type, const void* payload, uint32_t length, const int32_t* handles, uint32_t count) {
    uint8_t bytes[sizeof(swp_header) + sizeof(swp_commit_gpu)];
    if (length > sizeof(bytes) - sizeof(swp_header)) {
        return false;
    }

    swp_header header = { type, 0, length };
    memcpy(bytes, &header, sizeof(header));
    memcpy(bytes + sizeof(header), payload, length);

    return send_bytes(sock, bytes, sizeof(header) + length, handles, count);
}

static bool create_window(int sock, uint32_t win_id, const int32_t* handles, uint32_t count) {
    swp_create_window create;
    memset(&create, 0, sizeof(create));
    create.win_id = win_id;
    create.width = WINDOW_PIXELS;
    create.height = WINDOW_PIXELS;
    strncpy(create.title, "gputest", sizeof(create.title) - 1);

    return proto_send(sock, SWP_MSG_CREATE_WINDOW, &create, sizeof(create), handles, count);
}

bool proto_create_window(int sock, const int32_t* handles, uint32_t count) {
    return create_window(sock, PROTO_WINDOW_ID, handles, count);
}

bool proto_detach(int sock, uint32_t buf_id) {
    swp_detach_buffer detach = { PROTO_WINDOW_ID, buf_id };
    return proto_send(sock, SWP_MSG_DETACH_BUFFER, &detach, sizeof(detach), NULL, 0);
}

bool proto_closes(int sock) {
    uint64_t deadline = monotonic_ns() + WAIT_NS;
    uint8_t bytes[READ_CHUNK];

    while (monotonic_ns() < deadline) {
        struct pollfd pfd = { sock, POLLIN, 0 };
        if (poll(&pfd, 1, POLL_MS) <= 0) {
            continue;
        }

        ssize_t n = recv(sock, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            return true;
        }
    }

    return false;
}

static void fill_pipe_attach(attach_message* message) {
    memset(message, 0, sizeof(*message));
    message->header = (swp_header){ SWP_MSG_ATTACH_GPU_BUFFER, 0, sizeof(message->attach) };
    message->attach.win_id = PROTO_WINDOW_ID;
    message->attach.buf_id = 1;

    message->attach.width = WINDOW_PIXELS;
    message->attach.height = WINDOW_PIXELS;
    message->attach.pitch = WINDOW_PIXELS * sizeof(uint32_t);
    message->attach.format = SWP_FMT_XRGB8888;
}

// Whether no copy of the read end is left, as stlxdm closes a dropped client's handles before its socket
static bool pipe_has_no_reader(int write_end) {
    char byte = 0;
    return write(write_end, &byte, 1) < 0 && errno == EPIPE;
}

static bool send_handles(int sock, handle_send how, int32_t handle) {
    int32_t copies[HANDLES_MAX];
    for (uint32_t i = 0; i < HANDLES_MAX; i++) {
        copies[i] = handle;
    }

    attach_message message;
    fill_pipe_attach(&message);
    const uint8_t* bytes = (const uint8_t*)&message;
    uint16_t other_type = SWP_MSG_CREATE_WINDOW;

    uint8_t attach_then_byte[sizeof(message) + 1];
    memcpy(attach_then_byte, &message, sizeof(message));
    attach_then_byte[sizeof(message)] = bytes[0];

    switch (how) {
    case SEND_ATTACH:
        return proto_create_window(sock, NULL, 0) && send_bytes(sock, bytes, sizeof(message), copies, SWP_GPU_HANDLES);
    case SEND_CREATE_WINDOW:
        return proto_create_window(sock, copies, 1);
    case SEND_FOUR:
        return send_bytes(sock, bytes, 1, copies, SWP_GPU_HANDLES + 1);
    case SEND_OTHER_START:
        return send_bytes(sock, &other_type, sizeof(other_type), copies, 1);
    case SEND_SPLIT_ATTACH:
        return send_bytes(sock, bytes, 1, copies, SWP_GPU_HANDLES) && !proto_closes(sock) &&
               send_bytes(sock, bytes + 1, sizeof(message) - 1, NULL, 0);
    case SEND_THIRD_BATCH:
        return send_bytes(sock, bytes, 1, copies, SWP_GPU_HANDLES) &&
               send_bytes(sock, bytes + 1, 1, copies, SWP_GPU_HANDLES) && !proto_closes(sock) &&
               send_bytes(sock, bytes + 2, 1, copies, SWP_GPU_HANDLES);
    case SEND_SHORT_ATTACH:
        return send_bytes(sock, attach_then_byte, sizeof(attach_then_byte), copies, SWP_GPU_HANDLES - 1);
    }

    return false;
}

// Sends copies of a pipe's read end as `how` names, keeps only the write end, and checks the connection ends with no
// copy left open
static void check_disconnect(const char* name, handle_send how) {
    swp_gpu_info gpu_info;
    int ends[2] = { -1, -1 };
    int sock = proto_connect(&gpu_info);
    bool ready = sock >= 0 && pipe(ends) == 0;
    bool sent = ready && send_handles(sock, how, ends[0]);

    if (ends[0] >= 0) {
        close(ends[0]);
    }

    bool closed = sent && proto_closes(sock);
    check(name, closed && pipe_has_no_reader(ends[1]));

    if (ends[1] >= 0) {
        close(ends[1]);
    }

    if (sock >= 0) {
        close(sock);
    }
}

// Reads and drops `length` bytes, failing when the socket closes or `deadline` passes first
static bool skip_bytes(int sock, uint32_t length, uint64_t deadline) {
    uint8_t bytes[READ_CHUNK];
    while (length > 0) {
        uint32_t chunk = length < READ_CHUNK ? length : READ_CHUNK;
        if (!read_by(sock, bytes, chunk, deadline)) {
            return false;
        }

        length -= chunk;
    }

    return true;
}

// Asks for the clipboard, whose answer stlxdm sends only after handling every message sent before the ask, and drops
// what comes before the answer. Fails when the socket closes or 2 seconds pass first
static bool round_trip(int sock) {
    swp_header ask = { SWP_MSG_CLIPBOARD_GET, 0, 0 };
    if (!send_bytes(sock, &ask, sizeof(ask), NULL, 0)) {
        return false;
    }

    uint64_t deadline = monotonic_ns() + WAIT_NS;
    while (true) {
        swp_header header;
        if (!read_by(sock, &header, sizeof(header), deadline) || !skip_bytes(sock, header.length, deadline)) {
            return false;
        }

        if (header.type == SWP_MSG_CLIPBOARD_DATA) {
            return true;
        }
    }
}

static void shm_path(uint32_t serial, char* path, size_t size) {
    snprintf(path, size, SHM_DIR "gputest-%d-%u", (int)getpid(), serial);
}

// Unlinks the names of serials `first` to `last` that stlxdm never opened, which it leaves to gputest
static void unlink_shm_names(uint32_t first, uint32_t last) {
    for (uint32_t serial = first; serial <= last; serial++) {
        char path[SHM_PATH_MAX];
        shm_path(serial, path, sizeof(path));
        unlink(path);
    }
}

// Attaches a new shared memory object of one window's picture, named by `serial`, to window `win_id` as `buf_id`.
// stlxdm unlinks the name once it opens the object
static bool shm_attach(int sock, uint32_t win_id, uint32_t buf_id, uint32_t serial) {
    char path[SHM_PATH_MAX];
    shm_path(serial, path, sizeof(path));
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0);
    if (fd < 0) {
        return false;
    }

    bool sized = ftruncate(fd, WINDOW_PIXELS * WINDOW_PIXELS * sizeof(uint32_t)) == 0;
    close(fd);

    swp_attach_buffer attach = { win_id, buf_id, WINDOW_PIXELS, WINDOW_PIXELS, { 0 } };
    strncpy(attach.shm_name, path + strlen(SHM_DIR), sizeof(attach.shm_name) - 1);

    bool sent = sized && proto_send(sock, SWP_MSG_ATTACH_BUFFER, &attach, sizeof(attach), NULL, 0);
    if (!sent) {
        unlink(path);
    }

    return sent;
}

// Attaches and detaches a new shared memory object ATTACH_CYCLES times on the raw window
static bool cycles_shm_buffers(int sock) {
    uint32_t cycles = 0;
    uint32_t synced = 0;
    while (cycles < ATTACH_CYCLES && shm_attach(sock, PROTO_WINDOW_ID, 1, cycles) && proto_detach(sock, 1)) {
        cycles++;

        if (cycles % SHM_SYNC_EVERY == 0) {
            if (!round_trip(sock)) {
                break;
            }

            synced++;
        }
    }

    bool completed = cycles == ATTACH_CYCLES && synced == ATTACH_CYCLES / SHM_SYNC_EVERY;
    if (!completed) {
        unlink_shm_names(synced * SHM_SYNC_EVERY, cycles);
    }

    return completed;
}

// Attaches SWP_SHM_BUFFERS_MAX shared memory buffers across two raw windows and finds the connection still answering,
// then attaches one more, for which stlxdm must end the connection
static bool ends_past_shm_bound(int sock) {
    uint32_t attached = 0;
    while (attached < SWP_SHM_BUFFERS_MAX) {
        uint32_t win_id = PROTO_WINDOW_ID + attached % 2;
        if (!shm_attach(sock, win_id, attached + 1, ATTACH_CYCLES + attached)) {
            break;
        }

        attached++;
    }

    uint32_t past = ATTACH_CYCLES + SWP_SHM_BUFFERS_MAX;
    bool at_bound = attached == SWP_SHM_BUFFERS_MAX && round_trip(sock);
    bool ended = at_bound && shm_attach(sock, PROTO_WINDOW_ID, SWP_SHM_BUFFERS_MAX + 1, past) && proto_closes(sock);
    unlink_shm_names(ATTACH_CYCLES, past);

    return ended;
}

// Cycles shared memory buffers on one connection, then passes the bound on a second while the first stays open
static void check_shm_buffers(void) {
    swp_gpu_info gpu_info;
    int kept = proto_connect(&gpu_info);
    bool cycled = kept >= 0 && proto_create_window(kept, NULL, 0) && cycles_shm_buffers(kept);
    check("66000 shared memory attach and detach cycles on a new connection never run stlxdm out of handles", cycled);

    int full = proto_connect(&gpu_info);
    bool two_windows = full >= 0 && proto_create_window(full, NULL, 0) &&
                       create_window(full, PROTO_WINDOW_ID + 1, NULL, 0);
    bool ended = two_windows && ends_past_shm_bound(full);
    check(
        "an attach past SWP_SHM_BUFFERS_MAX shared memory buffers disconnects that connection alone",
        cycled && ended && round_trip(kept)
    );

    int socks[] = { kept, full };
    for (uint32_t i = 0; i < sizeof(socks) / sizeof(socks[0]); i++) {
        if (socks[i] >= 0) {
            close(socks[i]);
        }
    }
}

void run_proto_checks(void) {
    // A write to a pipe with no reader would otherwise end gputest instead of failing with EPIPE
    signal(SIGPIPE, SIG_IGN);

    int gpu = open("/dev/gpu0", O_RDWR);
    bool has_gpu = gpu >= 0;
    if (has_gpu) {
        close(gpu);
    }

    swp_gpu_info gpu_info = { ~0u, ~0u };
    int sock = proto_connect(&gpu_info);
    uint32_t reader = gpu_info.flags & ~(uint32_t)SWP_GPU_ACCEPTS;
    bool names_one_reader = (gpu_info.flags & SWP_GPU_ACCEPTS) &&
                            (reader == SWP_GPU_CPU_READS || reader == SWP_GPU_GPU_READS);
    bool as_expected = (has_gpu ? names_one_reader : gpu_info.flags == 0) && gpu_info.device == 0;
    check("the handshake's GPU_INFO offers GPU buffers, naming one reader, exactly when /dev/gpu0 opens",
          sock >= 0 && as_expected);

    if (sock >= 0) {
        close(sock);
    }

    check_disconnect("an attach of three pipe handles disconnects and stlxdm closes them", SEND_ATTACH);
    check_disconnect("a handle sent with CREATE_WINDOW disconnects and stlxdm closes it", SEND_CREATE_WINDOW);
    check_disconnect("four handles in one sendmsg disconnect and leave no copy open", SEND_FOUR);

    check_disconnect(
        "a handle sent with the start of another message disconnects and stlxdm closes it",
        SEND_OTHER_START
    );
    check_disconnect(
        "an attach's handles sent with its first byte keep the connection open until its rest, and stlxdm closes them",
        SEND_SPLIT_ATTACH
    );
    check_disconnect(
        "two batches of handles for one attach keep the connection open, a third disconnects, and stlxdm closes them",
        SEND_THIRD_BATCH
    );
    check_disconnect("an attach with two handles disconnects and stlxdm closes them", SEND_SHORT_ATTACH);

    check_shm_buffers();
}
