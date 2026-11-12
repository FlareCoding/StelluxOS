/* offer_intake, run on the build machine: received handles queue in order over a real socket pair, an offer takes the
 * oldest of them and keeps only regular objects that hold their lengths, and a small item moves into objects of its
 * own */

#include "offer_intake.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;
constexpr size_t CONTROL_BYTES = CMSG_SPACE(SWP_HANDLES_MAX * sizeof(int));

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static bool is_open(int object) {
    return fcntl(object, F_GETFD) != -1;
}

/* A regular file holding `text`, which reads and maps as a shared memory object does */
static int object_holding(std::string_view text) {
    char path[] = "/tmp/stlxdm-intake-XXXXXX";
    int object = mkstemp(path);
    if (object < 0) {
        return -1;
    }

    unlink(path);
    if (write(object, text.data(), text.size()) != static_cast<ssize_t>(text.size())) {
        close(object);
        return -1;
    }

    return object;
}

static std::string contents_of(int object) {
    char bytes[256];
    ssize_t n = pread(object, bytes, sizeof(bytes), 0);
    return n > 0 ? std::string(bytes, static_cast<size_t>(n)) : std::string();
}

/* Sends one byte carrying `handles` */
static bool send_handles(int fd, std::initializer_list<int> handles) {
    char byte = 0;
    iovec iov = { &byte, 1 };
    alignas(cmsghdr) uint8_t control[CONTROL_BYTES] = {};

    msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(handles.size() * sizeof(int));

    cmsghdr* head = CMSG_FIRSTHDR(&msg);
    head->cmsg_level = SOL_SOCKET;
    head->cmsg_type = SCM_RIGHTS;
    head->cmsg_len = CMSG_LEN(handles.size() * sizeof(int));
    memcpy(CMSG_DATA(head), handles.begin(), handles.size() * sizeof(int));

    return sendmsg(fd, &msg, 0) == 1;
}

/* Reads one byte and queues its handles, with room in the read for exactly `room` of them, as the server sizes its
 * reads, since CMSG_SPACE's padding would fit one more on some systems */
static bool receive_into(int fd, handle_queue queue, size_t room) {
    char byte;
    iovec iov = { &byte, 1 };
    alignas(cmsghdr) uint8_t control[CONTROL_BYTES] = {};

    msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = CMSG_LEN(room * sizeof(int));

    return recvmsg(fd, &msg, 0) == 1 && queue_received_handles(queue, msg);
}

static void check_received_handles() {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        check(false, "the test makes its socket pair");
        return;
    }

    int handles[3] = {};
    uint32_t count = 0;
    handle_queue queue = { handles, 3, &count };

    int first = object_holding("first");
    int second = object_holding("second");
    bool is_received = send_handles(pair[0], { first, second }) && receive_into(pair[1], queue, SWP_HANDLES_MAX);
    check(is_received && count == 2, "a read's handles queue");
    check(contents_of(handles[0]) == "first" && contents_of(handles[1]) == "second",
          "handles queue in the order they were sent");

    int third = object_holding("third");
    int fourth = object_holding("fourth");
    bool is_kept = send_handles(pair[0], { third, fourth }) && receive_into(pair[1], queue, SWP_HANDLES_MAX);
    check(!is_kept && count == 3 && contents_of(handles[2]) == "third",
          "handles past the queue's room are refused, those that fit kept");

    count = 0;
    bool is_whole = send_handles(pair[0], { first, second }) && receive_into(pair[1], queue, 1);
    check(!is_whole, "handles the kernel truncates are refused");

    for (int object : { first, second, third, fourth, pair[0], pair[1] }) {
        close(object);
    }
}

static void check_handle_messages() {
    uint16_t offer = SWP_MSG_CLIPBOARD_OFFER;
    uint16_t hello = SWP_MSG_HELLO;
    uint8_t bytes[2];

    memcpy(bytes, &offer, sizeof(offer));
    check(may_begin_handle_message(bytes, 2), "an offer may take queued handles");
    memcpy(bytes, &hello, sizeof(hello));
    check(!may_begin_handle_message(bytes, 2), "a hello takes no handles");
    check(may_begin_handle_message(bytes, 1), "a lone byte may yet begin a message that takes handles");
    check(!may_begin_handle_message(bytes, 0), "no bytes begin no message");
}

static void check_offer_objects() {
    int extra = object_holding("extra");
    int handles[3] = { object_holding("hello"), object_holding("png"), extra };
    uint32_t count = 3;
    std::vector<clipboard_rep> reps = { { SWP_MIME_TEXT, 5, -1 }, { "text/html", 0, -1 }, { "image/png", 3, -1 } };
    intake_error error = take_offer_objects(reps, { handles, 3, &count });

    check(error == intake_error::none, "an offer whose objects hold their lengths is kept");
    check(contents_of(reps[0].object) == "hello" && reps[1].object == -1 && contents_of(reps[2].object) == "png",
          "each representation with bytes takes the oldest handle, and one without takes none");
    check(count == 1 && handles[0] == extra, "the handles an offer takes leave the queue");
    check(reps[0].held == 5 && reps[2].held == 3, "each representation records what its object holds");

    int missing[1] = { object_holding("only") };
    uint32_t one = 1;
    std::vector<clipboard_rep> two = { { SWP_MIME_TEXT, 4, -1 }, { "text/html", 4, -1 } };
    check(take_offer_objects(two, { missing, 1, &one }) == intake_error::missing_handles && one == 1,
          "an offer missing a handle is refused, taking none");

    int short_object = object_holding("ab");
    uint32_t short_count = 1;
    std::vector<clipboard_rep> claimed = { { SWP_MIME_TEXT, 5, -1 } };
    check(take_offer_objects(claimed, { &short_object, 1, &short_count }) == intake_error::short_object,
          "an object holding less than its length is refused");
    check(!is_open(short_object) && claimed[0].object == -1, "a refused offer closes the handles it took");

    int ends[2];
    int sockets[2];
    if (pipe(ends) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        check(false, "the test makes its pipe and sockets");
        return;
    }

    uint32_t pipe_count = 1;
    std::vector<clipboard_rep> piped = { { SWP_MIME_TEXT, 1, -1 } };
    check(take_offer_objects(piped, { &ends[0], 1, &pipe_count }) == intake_error::not_memory && !is_open(ends[0]),
          "a pipe is refused as an object");

    uint32_t socket_count = 1;
    std::vector<clipboard_rep> socketed = { { SWP_MIME_TEXT, 1, -1 } };
    check(take_offer_objects(socketed, { &sockets[0], 1, &socket_count }) == intake_error::not_memory,
          "a socket, such as the sender's own connection, is refused as an object");

    int large = object_holding(std::string(4096, 'x'));
    uint32_t large_count = 1;
    std::vector<clipboard_rep> lying = { { SWP_MIME_TEXT, 1, -1 } };
    check(take_offer_objects(lying, { &large, 1, &large_count }) == intake_error::none && lying[0].held == 4096,
          "an object longer than its length is kept, counted at what it holds");

    close(lying[0].object);
    close(extra);
    for (const clipboard_rep& rep : reps) {
        if (rep.object >= 0) {
            close(rep.object);
        }
    }

    for (int fd : { ends[1], sockets[1], missing[0] }) {
        close(fd);
    }
}

static int make_regular(const void* data, size_t len) {
    return object_holding(std::string_view(static_cast<const char*>(data), len));
}

static int make_nothing(const void*, size_t) {
    return -1;
}

static void check_owned_items() {
    int offered = object_holding("hello");
    std::vector<clipboard_rep> small = { { SWP_MIME_TEXT, 5, offered, 5 }, { "text/html", 0, -1 } };
    own_small_item(small, make_regular);
    check(small[0].object >= 0 && small[0].object != offered && !is_open(offered),
          "a small item moves into an object of its own, the offered one closed");
    check(contents_of(small[0].object) == "hello" && small[0].held == 5 && small[1].object == -1,
          "the owned object holds the same bytes, and a representation without bytes stays without an object");

    int big = object_holding(std::string(OWNED_COPY_MAX + 1, 'y'));
    std::vector<clipboard_rep> large = { { SWP_MIME_TEXT, OWNED_COPY_MAX + 1, big, OWNED_COPY_MAX + 1 } };
    own_small_item(large, make_regular);
    check(large[0].object == big && is_open(big), "a larger item stays in its offered object");

    int kept = object_holding("kept");
    std::vector<clipboard_rep> refused = { { SWP_MIME_TEXT, 4, kept, 4 } };
    own_small_item(refused, make_nothing);
    check(refused[0].object == kept && is_open(kept), "an item whose copy finds no memory stays as offered");

    for (int object : { small[0].object, big, kept }) {
        close(object);
    }
}

int main() {
    check_received_handles();
    check_handle_messages();
    check_offer_objects();
    check_owned_items();

    if (g_failures != 0) {
        printf("offer_intake: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("offer_intake: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
