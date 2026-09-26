#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "../net/stub_interface.h"
#include "syscall/handlers/sys_socket.h"
#include "syscall/handlers/sys_error_map.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_io.h"
#include "resource/resource.h"
#include "resource/socket_ops.h"
#include "net/inet.h"
#include "net/udp_socket.h"
#include "net/tcp/socket.h"
#include "net/tcp/conn.h"
#include "net/udp.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;
using test_helpers::spin_wait;
using namespace net;

TEST_SUITE(socket_syscall);

constexpr uint16_t STREAM_PORT  = 50020;
constexpr size_t   STREAM_BYTES = 40000; // Past two staging rounds and past a 16 KiB queue
constexpr size_t   STREAM_PAGES = 10;

static const ipv4::ipv4_addr g_loopback = {{127, 0, 0, 1}};

// Offsets of the pieces a message call needs inside one user page
constexpr size_t MSG_HDR   = 0;
constexpr size_t MSG_IOVS  = 64;
constexpr size_t MSG_NAME  = 128;
constexpr size_t MSG_BUF_A = 256;
constexpr size_t MSG_BUF_B = 512;

constexpr uint16_t TEST_PORT = 50010;
constexpr uint16_t PEER_PORT = 40000;

static const ipv4::ipv4_addr g_peer = {{10, 0, 2, 2}};
static const ipv4::ipv4_addr g_host = {{10, 0, 2, 15}};

// Mirrors the userland layouts the handlers copy in
struct user_iovec {
    uint64_t base;
    uint64_t len;
};

struct user_msghdr {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad0;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    uint32_t flags;
    uint32_t pad1;
};

// Close-on-exec from the handle together with its object's status flags
static uint32_t handle_flags_of(sched::task* task, int64_t h) {
    resource::resource_object* obj = nullptr;
    uint32_t flags = 0;
    if (resource::get_handle_object(task->handles, static_cast<resource::handle_t>(h),
                                    resource::RIGHT_READ, &obj, &flags) == resource::HANDLE_OK) {
        flags |= resource::get_status_flags(obj);
        resource::resource_release(obj);
    }

    return flags;
}

static udp::udp_socket* udp_socket_of(sched::task* task, int64_t h) {
    resource::resource_object* obj = nullptr;
    if (resource::get_handle_object(task->handles, static_cast<resource::handle_t>(h),
                                    resource::RIGHT_READ, &obj) != resource::HANDLE_OK) {
        return nullptr;
    }

    auto* sock = static_cast<udp::udp_socket*>(obj->impl);
    resource::resource_release(obj);
    return sock;
}

// A datagram as udp::input hands it to the sockets, carrying `payload`
static packet* make_datagram(uint16_t dst_port, const char* payload, size_t payload_len) {
    packet* pkt = packet::alloc();
    if (!pkt) {
        return nullptr;
    }

    (void)pkt->reserve(eth::HEADER_LEN);

    auto* ip = reinterpret_cast<ipv4::ipv4_header*>(pkt->put(ipv4::HEADER_LEN));
    ip->set_version_ihl(ipv4::VERSION, ipv4::MIN_IHL);
    ip->proto = ipv4::PROTO_UDP;
    ip->src = g_peer;
    ip->dst = g_host;
    pkt->mark_network_header();

    auto* hdr = reinterpret_cast<udp::udp_header*>(pkt->put(udp::HEADER_LEN));
    hdr->src_port = htons(PEER_PORT);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons(static_cast<uint16_t>(udp::HEADER_LEN + payload_len));
    string::memcpy(pkt->put(payload_len), payload, payload_len);

    (void)pkt->pull(ipv4::HEADER_LEN);
    pkt->mark_transport_header();
    return pkt;
}

// Two buffers with a legal empty null slot between them
static void lay_out_message(user_page& page, uint32_t namelen, size_t len_a, size_t len_b) {
    user_iovec* iovs = page.at<user_iovec>(MSG_IOVS);
    iovs[0] = {page.addr + MSG_BUF_A, len_a};
    iovs[1] = {0, 0};
    iovs[2] = {page.addr + MSG_BUF_B, len_b};

    user_msghdr* hdr = page.at<user_msghdr>(MSG_HDR);
    *hdr = {};
    hdr->name = page.addr + MSG_NAME;
    hdr->namelen = namelen;
    hdr->iov = page.addr + MSG_IOVS;
    hdr->iovlen = 3;
}

TEST(socket_syscall, creation_flags_reach_the_new_socket) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t plain = sys_socket(net::inet::AF_INET, net::inet::SOCK_DGRAM, 0, 0, 0, 0);
    int64_t flagged = sys_socket(net::inet::AF_INET,
                                 net::inet::SOCK_DGRAM | fs::O_NONBLOCK | fs::O_CLOEXEC, 0, 0, 0, 0);
    ASSERT_TRUE(plain >= 0);
    ASSERT_TRUE(flagged >= 0);

    EXPECT_EQ(handle_flags_of(task, plain), 0u);
    EXPECT_EQ(handle_flags_of(task, flagged), fs::O_NONBLOCK | resource::RESOURCE_HANDLE_CLOEXEC);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(plain)), resource::OK);
    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(flagged)), resource::OK);
}

TEST(socket_syscall, unknown_type_bits_are_still_rejected) {
    int64_t rc = sys_socket(net::inet::AF_INET, net::inet::SOCK_DGRAM | 0x10, 0, 0, 0, 0);
    EXPECT_EQ(rc, syscall::EPROTONOSUPPORT);
}

TEST(socket_syscall, a_send_on_a_broken_connection_reports_epipe) {
    EXPECT_EQ(syscall::error_map::map_socket_op_error(resource::ERR_PIPE), syscall::EPIPE);
}

// --- sendmsg_gathers_the_vector_into_one_datagram ---
// Proves: the pieces of an iovec leave as a single datagram, addressed by msg_name.

TEST(socket_syscall, sendmsg_gathers_the_vector_into_one_datagram) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    stub_interface link;
    int64_t fd = sys_socket(inet::AF_INET, inet::SOCK_DGRAM, 0, 0, 0, 0);
    ASSERT_TRUE(fd >= 0);

    udp::udp_socket* sock = udp_socket_of(task, fd);
    ASSERT_NOT_NULL(sock);
    sock->iface = &link;
    sock->broadcast_allowed = true;

    user_page page;
    ASSERT_TRUE(page.ready());
    string::memcpy(page.at<char>(MSG_BUF_A), "dhcp", 4);
    string::memcpy(page.at<char>(MSG_BUF_B), "disc", 4);
    lay_out_message(page, inet::SOCKADDR_IN_LEN, 4, 4);
    *page.at<inet::sockaddr_in>(MSG_NAME) = {inet::AF_INET, htons(67), ipv4::BROADCAST_ADDR, {}};

    int64_t sent = 0;
    {
        user_space_scope scope(page.ctx);
        sent = sys_sendmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, 0, 0, 0, 0);
    }
    EXPECT_EQ(sent, static_cast<int64_t>(8));
    ASSERT_EQ(link.frames_sent(), static_cast<size_t>(1));

    const uint8_t* frame = link.last_frame();
    const auto* hdr = reinterpret_cast<const udp::udp_header*>(frame + eth::HEADER_LEN + ipv4::HEADER_LEN);
    EXPECT_EQ(ntohs(hdr->dst_port), 67);
    EXPECT_EQ(ntohs(hdr->length), static_cast<uint16_t>(udp::HEADER_LEN + 8));
    EXPECT_EQ(string::memcmp(frame + eth::HEADER_LEN + ipv4::HEADER_LEN + udp::HEADER_LEN, "dhcpdisc", 8), 0);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(fd)), resource::OK);
}

// --- recvmsg_scatters_a_datagram_and_reports_its_source ---
// Proves: one datagram fills the iovec in order, msg_name receives the sender,
// and the header comes back with no ancillary data and no flags.

TEST(socket_syscall, recvmsg_scatters_a_datagram_and_reports_its_source) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t fd = sys_socket(inet::AF_INET, inet::SOCK_DGRAM, 0, 0, 0, 0);
    ASSERT_TRUE(fd >= 0);

    udp::udp_socket* sock = udp_socket_of(task, fd);
    ASSERT_NOT_NULL(sock);
    ASSERT_EQ(udp::socket_bind(sock, ipv4::UNSPECIFIED_ADDR, TEST_PORT), OK);

    packet* pkt = make_datagram(TEST_PORT, "hello world", 11);
    ASSERT_NOT_NULL(pkt);
    ASSERT_EQ(udp::socket_deliver(pkt), OK);

    user_page page;
    ASSERT_TRUE(page.ready());
    lay_out_message(page, inet::SOCKADDR_IN_LEN, 5, 32);

    int64_t received = 0;
    int64_t empty = 0;
    {
        user_space_scope scope(page.ctx);
        received = sys_recvmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, inet::MSG_DONTWAIT, 0, 0, 0);
        empty = sys_recvmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, inet::MSG_DONTWAIT, 0, 0, 0);
    }
    EXPECT_EQ(received, static_cast<int64_t>(11));
    EXPECT_EQ(empty, syscall::EAGAIN);
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_A), "hello", 5), 0);
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_B), " world", 6), 0);

    const user_msghdr* hdr = page.at<user_msghdr>(MSG_HDR);
    EXPECT_EQ(hdr->namelen, static_cast<uint32_t>(inet::SOCKADDR_IN_LEN));
    EXPECT_EQ(hdr->controllen, static_cast<uint64_t>(0));
    EXPECT_EQ(hdr->flags, 0u);

    const auto* source = page.at<inet::sockaddr_in>(MSG_NAME);
    EXPECT_EQ(source->family, inet::AF_INET);
    EXPECT_EQ(ntohs(source->port), PEER_PORT);
    EXPECT_TRUE(source->addr == g_peer);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(fd)), resource::OK);
}

// Several eager user pages, each reachable from the kernel through its frame
struct user_region {
    mm::mm_context* ctx = nullptr;
    uintptr_t addr = 0;
    uint8_t* pages[STREAM_PAGES] = {};

    user_region() {
        ctx = mm::mm_context_create();
        if (!ctx) {
            return;
        }

        uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
        uint32_t flags = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;
        if (mm::mm_context_map_anonymous(ctx, 0, STREAM_PAGES * pmm::PAGE_SIZE, prot, flags, &addr) != mm::MM_CTX_OK) {
            addr = 0;
            return;
        }

        for (size_t i = 0; i < STREAM_PAGES; i++) {
            pmm::phys_addr_t phys = paging::get_physical(addr + i * pmm::PAGE_SIZE, ctx->pt_root);
            pages[i] = phys ? static_cast<uint8_t*>(paging::phys_to_virt(phys)) : nullptr;
        }
    }

    ~user_region() {
        if (ctx) {
            mm::mm_context_release(ctx);
        }
    }

    bool ready() const {
        for (size_t i = 0; i < STREAM_PAGES; i++) {
            if (!pages[i]) {
                return false;
            }
        }

        return addr != 0;
    }

    uint8_t& byte(size_t offset) { return pages[offset / pmm::PAGE_SIZE][offset % pmm::PAGE_SIZE]; }
};

static uint8_t stream_pattern(size_t index) {
    return static_cast<uint8_t>(index * 7 + 3);
}

enum class stream_call : uint8_t {
    send    = 0,
    write   = 1,
    receive = 2,
};

// The client side runs in an elevated task of its own, since its system calls
// block and the runner is the idle task, which cannot
struct stream_client_run {
    mm::mm_context*        ctx;
    uintptr_t              buf;
    uintptr_t              addr;
    stream_call            call;
    int64_t                result;
    sync::atomic<uint32_t> done;
};

static stream_client_run g_stream_client;
static uint8_t           g_stream_scratch[16384];

static tcp::tcp_socket* tcp_socket_of(sched::task* task, int64_t h) {
    resource::resource_object* obj = nullptr;
    if (resource::get_handle_object(task->handles, static_cast<resource::handle_t>(h),
                                    resource::RIGHT_READ, &obj) != resource::HANDLE_OK) {
        return nullptr;
    }

    auto* sock = static_cast<tcp::tcp_socket*>(obj->impl);
    resource::resource_release(obj);
    return sock;
}

// Connects, moves STREAM_BYTES in one system call, and closes in order so the
// bytes still queued reach the other end ahead of the FIN
static void run_stream_client(void*) {
    stream_client_run& run = g_stream_client;
    sched::task* self = sched::current();
    int64_t fd = sys_socket(inet::AF_INET, inet::SOCK_STREAM, 0, 0, 0, 0);
    run.result = fd;

    if (fd >= 0) {
        {
            user_space_scope scope(run.ctx);
            run.result = sys_connect(static_cast<uint64_t>(fd), run.addr, inet::SOCKADDR_IN_LEN, 0, 0, 0);
            if (run.result == 0 && run.call == stream_call::send) {
                run.result = sys_sendto(static_cast<uint64_t>(fd), run.buf, STREAM_BYTES, 0, 0, 0);
            } else if (run.result == 0 && run.call == stream_call::write) {
                run.result = sys_write(static_cast<uint64_t>(fd), run.buf, STREAM_BYTES, 0, 0, 0);
            } else if (run.result == 0) {
                run.result = sys_recvfrom(static_cast<uint64_t>(fd), run.buf, STREAM_BYTES, inet::MSG_WAITALL, 0, 0);
            }
        }

        (void)resource::close(self, static_cast<resource::handle_t>(fd));
    }

    run.done.store_release(1);
    sched::exit(0);
}

// Moves STREAM_BYTES through the server's ops without ever blocking the runner
static bool serve_stream(resource::resource_object* server, bool sending) {
    const resource::socket_ops* ops = server->ops->socket;
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    size_t moved = 0;
    bool intact = true;

    while (moved < STREAM_BYTES && clock::now_ns() < deadline) {
        size_t chunk = STREAM_BYTES - moved < sizeof(g_stream_scratch) ? STREAM_BYTES - moved : sizeof(g_stream_scratch);
        ssize_t n;
        if (sending) {
            for (size_t i = 0; i < chunk; i++) {
                g_stream_scratch[i] = stream_pattern(moved + i);
            }

            n = ops->sendto(server, g_stream_scratch, chunk, inet::MSG_DONTWAIT, nullptr, 0);
        } else {
            n = ops->recvfrom(server, g_stream_scratch, chunk, inet::MSG_DONTWAIT, nullptr, nullptr);
            for (ssize_t i = 0; i < n; i++) {
                intact = intact && g_stream_scratch[i] == stream_pattern(moved + i);
            }
        }

        if (n == resource::ERR_AGAIN) {
            continue;
        }

        if (n <= 0) {
            return false;
        }

        moved += static_cast<size_t>(n);
    }

    return moved == STREAM_BYTES && intact;
}

// A listener on the loopback address, its accepted connection polled for and
// aborted with it, so nothing outlives the case
struct loopback_listener {
    sched::task* task;
    int64_t      fd = -1;
    int64_t      accepted = -1;

    explicit loopback_listener(sched::task* runner) : task(runner) {
        fd = sys_socket(inet::AF_INET, inet::SOCK_STREAM | fs::O_NONBLOCK, 0, 0, 0, 0);
        if (fd < 0) {
            return;
        }

        tcp::tcp_socket* sock = tcp_socket_of(task, fd);
        if (tcp::socket_bind(sock, g_loopback, STREAM_PORT) != OK || tcp::socket_listen(sock, 1) != OK) {
            (void)resource::close(task, static_cast<resource::handle_t>(fd));
            fd = -1;
        }
    }

    ~loopback_listener() {
        tcp::tcp_socket* sock = accepted >= 0 ? tcp_socket_of(task, accepted) : nullptr;
        if (sock && sock->conn) {
            tcp::abort_connection(sock->conn.ptr());
        }

        if (accepted >= 0) {
            (void)resource::close(task, static_cast<resource::handle_t>(accepted));
        }

        if (fd >= 0) {
            (void)resource::close(task, static_cast<resource::handle_t>(fd));
        }
    }

    resource::resource_object* accept() {
        uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
        while (accepted < 0 && clock::now_ns() < deadline) {
            accepted = sys_accept(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
        }

        resource::resource_object* obj = nullptr;
        if (accepted >= 0) {
            (void)resource::get_handle_object(task->handles, static_cast<resource::handle_t>(accepted),
                                              resource::RIGHT_READ, &obj);
        }

        return obj;
    }
};

// A nonblocking client the runner itself owns, connected over the loopback
// interface, so its calls may run on the runner as long as none of them waits
struct loopback_client {
    sched::task* task;
    int64_t      fd = -1;

    explicit loopback_client(sched::task* runner) : task(runner) {
        fd = sys_socket(inet::AF_INET, inet::SOCK_STREAM | fs::O_NONBLOCK, 0, 0, 0, 0);
        if (fd < 0) {
            return;
        }

        resource::resource_object* obj = nullptr;
        if (resource::get_handle_object(task->handles, static_cast<resource::handle_t>(fd),
                                        resource::RIGHT_WRITE, &obj) != resource::HANDLE_OK) {
            return;
        }

        inet::sockaddr_in addr = {inet::AF_INET, htons(STREAM_PORT), g_loopback, {}};
        (void)obj->ops->socket->connect(obj, &addr, sizeof(addr), true);
        resource::resource_release(obj);

        uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
        while (!ready() && clock::now_ns() < deadline) {
        }
    }

    ~loopback_client() {
        tcp::tcp_conn* established = conn();
        if (established) {
            tcp::abort_connection(established);
        }

        if (fd >= 0) {
            (void)resource::close(task, static_cast<resource::handle_t>(fd));
        }
    }

    tcp::tcp_conn* conn() const {
        tcp::tcp_socket* sock = fd >= 0 ? tcp_socket_of(task, fd) : nullptr;
        return sock && sock->conn ? sock->conn.ptr() : nullptr;
    }

    bool ready() const {
        tcp::tcp_conn* established = conn();
        if (!established) {
            return false;
        }

        bool up = false;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(established->lock);
            up = established->state == tcp::tcp_state::established;
        });
        return up;
    }

    size_t queued() const {
        tcp::tcp_conn* established = conn();
        size_t bytes = 0;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(established->lock);
            bytes = established->rcv_queue.size();
        });
        return bytes;
    }
};

TEST(socket_syscall, a_nonblocking_stream_send_returns_what_fit_rather_than_waiting) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_region region;
    ASSERT_TRUE(region.ready());
    loopback_listener listener(task);
    ASSERT_TRUE(listener.fd >= 0);
    loopback_client client(task);
    ASSERT_TRUE(client.ready());

    for (size_t i = 0; i < STREAM_BYTES; i++) {
        region.byte(i) = stream_pattern(i);
    }

    int64_t sent = 0;
    int64_t again = 0;
    {
        user_space_scope scope(region.ctx);
        sent = sys_sendto(static_cast<uint64_t>(client.fd), region.addr, STREAM_BYTES, 0, 0, 0);
        again = sys_sendto(static_cast<uint64_t>(client.fd), region.addr, STREAM_BYTES, 0, 0, 0);
    }

    EXPECT_EQ(sent, static_cast<int64_t>(tcp::SND_CHUNKS_INITIAL * tcp::CHUNK_PAYLOAD));
    EXPECT_TRUE(again == syscall::EAGAIN || (again > 0 && again <= static_cast<int64_t>(syscall::STREAM_CHUNK_SIZE)));
}

TEST(socket_syscall, a_receive_that_faults_leaves_the_bytes_queued_for_the_next_call) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_region region;
    ASSERT_TRUE(region.ready());
    loopback_listener listener(task);
    ASSERT_TRUE(listener.fd >= 0);
    loopback_client client(task);
    ASSERT_TRUE(client.ready());
    resource::resource_object* server = listener.accept();
    ASSERT_NOT_NULL(server);

    for (size_t i = 0; i < 300; i++) {
        g_stream_scratch[i] = stream_pattern(i);
    }

    EXPECT_EQ(server->ops->socket->sendto(server, g_stream_scratch, 300, inet::MSG_DONTWAIT, nullptr, 0), 300);
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (client.queued() < 300 && clock::now_ns() < deadline) {
    }
    ASSERT_EQ(client.queued(), 300u);

    // A buffer whose last pages are not mapped: the copy faults after the peek
    uintptr_t torn = region.addr + STREAM_PAGES * pmm::PAGE_SIZE - 16;
    int64_t faulted = 0;
    int64_t got = 0;
    {
        user_space_scope scope(region.ctx);
        faulted = sys_recvfrom(static_cast<uint64_t>(client.fd), torn, 300, inet::MSG_DONTWAIT, 0, 0);
        got = sys_recvfrom(static_cast<uint64_t>(client.fd), region.addr, 300, inet::MSG_DONTWAIT, 0, 0);
    }

    EXPECT_EQ(faulted, syscall::EFAULT);
    EXPECT_EQ(got, static_cast<int64_t>(300));
    bool intact = true;
    for (size_t i = 0; i < 300; i++) {
        intact = intact && region.byte(i) == stream_pattern(i);
    }
    EXPECT_TRUE(intact);

    resource::resource_release(server);
}

TEST(socket_syscall, a_peek_through_the_system_call_leaves_the_bytes_queued) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_region region;
    ASSERT_TRUE(region.ready());
    loopback_listener listener(task);
    ASSERT_TRUE(listener.fd >= 0);
    loopback_client client(task);
    ASSERT_TRUE(client.ready());
    resource::resource_object* server = listener.accept();
    ASSERT_NOT_NULL(server);

    for (size_t i = 0; i < 300; i++) {
        g_stream_scratch[i] = stream_pattern(i);
    }

    EXPECT_EQ(server->ops->socket->sendto(server, g_stream_scratch, 300, inet::MSG_DONTWAIT, nullptr, 0), 300);
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (client.queued() < 300 && clock::now_ns() < deadline) {
    }
    ASSERT_EQ(client.queued(), 300u);

    int64_t peeked = 0;
    size_t still_queued = 0;
    int64_t got = 0;
    {
        user_space_scope scope(region.ctx);
        peeked = sys_recvfrom(static_cast<uint64_t>(client.fd), region.addr, 300,
                              inet::MSG_PEEK | inet::MSG_DONTWAIT, 0, 0);
        still_queued = client.queued();
        got = sys_recvfrom(static_cast<uint64_t>(client.fd), region.addr + 1024, 300, inet::MSG_DONTWAIT, 0, 0);
    }

    EXPECT_EQ(peeked, static_cast<int64_t>(300));
    EXPECT_EQ(still_queued, 300u);
    EXPECT_EQ(got, static_cast<int64_t>(300));
    bool intact = true;
    for (size_t i = 0; i < 300; i++) {
        intact = intact && region.byte(i) == stream_pattern(i) && region.byte(1024 + i) == stream_pattern(i);
    }
    EXPECT_TRUE(intact);

    resource::resource_release(server);
}

TEST(socket_syscall, a_discarding_receive_drops_the_bytes_without_writing_the_buffer) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_region region;
    ASSERT_TRUE(region.ready());
    loopback_listener listener(task);
    ASSERT_TRUE(listener.fd >= 0);
    loopback_client client(task);
    ASSERT_TRUE(client.ready());
    resource::resource_object* server = listener.accept();
    ASSERT_NOT_NULL(server);

    for (size_t i = 0; i < 300; i++) {
        g_stream_scratch[i] = stream_pattern(i);
        region.byte(i) = 0xAA;
    }

    EXPECT_EQ(server->ops->socket->sendto(server, g_stream_scratch, 300, inet::MSG_DONTWAIT, nullptr, 0), 300);
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (client.queued() < 300 && clock::now_ns() < deadline) {
    }
    ASSERT_EQ(client.queued(), 300u);

    int64_t dropped = 0;
    size_t still_queued = 0;
    int64_t empty = 0;
    {
        user_space_scope scope(region.ctx);
        dropped = sys_recvfrom(static_cast<uint64_t>(client.fd), region.addr, 300,
                               inet::MSG_TRUNC | inet::MSG_DONTWAIT, 0, 0);
        still_queued = client.queued();
        empty = sys_recvfrom(static_cast<uint64_t>(client.fd), region.addr, 300, inet::MSG_DONTWAIT, 0, 0);
    }

    EXPECT_EQ(dropped, static_cast<int64_t>(300));
    EXPECT_EQ(still_queued, 0u);
    EXPECT_EQ(empty, syscall::EAGAIN);
    bool untouched = true;
    for (size_t i = 0; i < 300; i++) {
        untouched = untouched && region.byte(i) == 0xAA;
    }
    EXPECT_TRUE(untouched);

    resource::resource_release(server);
}

// The client connects and moves STREAM_BYTES in one system call while the
// runner serves the other end
static void run_stream_case(stream_call call) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_region region;
    ASSERT_TRUE(region.ready());
    loopback_listener listener(task);
    ASSERT_TRUE(listener.fd >= 0);

    bool client_sends = call != stream_call::receive;
    for (size_t i = 0; i < STREAM_BYTES; i++) {
        region.byte(i) = client_sends ? stream_pattern(i) : 0;
    }

    inet::sockaddr_in addr = {inet::AF_INET, htons(STREAM_PORT), g_loopback, {}};
    string::memcpy(&region.byte(STREAM_BYTES), &addr, sizeof(addr));

    g_stream_client.ctx = region.ctx;
    g_stream_client.buf = region.addr;
    g_stream_client.addr = region.addr + STREAM_BYTES;
    g_stream_client.call = call;
    g_stream_client.result = 0;
    g_stream_client.done.store_relaxed(0);
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(run_stream_client, nullptr, "stream_client", sched::TASK_FLAG_ELEVATED);
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    resource::resource_object* server = listener.accept();
    EXPECT_TRUE(server != nullptr);
    EXPECT_TRUE(server && serve_stream(server, !client_sends));
    EXPECT_TRUE(spin_wait(g_stream_client.done));
    EXPECT_EQ(g_stream_client.result, static_cast<int64_t>(STREAM_BYTES));

    if (!client_sends) {
        bool intact = true;
        for (size_t i = 0; i < STREAM_BYTES; i++) {
            intact = intact && region.byte(i) == stream_pattern(i);
        }

        EXPECT_TRUE(intact);
    }

    if (server) {
        resource::resource_release(server);
    }
}

TEST(socket_syscall, a_stream_send_past_one_staging_round_finishes_in_one_call) {
    run_stream_case(stream_call::send);
}

TEST(socket_syscall, a_stream_write_past_one_staging_round_finishes_in_one_call) {
    run_stream_case(stream_call::write);
}

TEST(socket_syscall, a_stream_receive_with_waitall_gathers_past_one_staging_round) {
    run_stream_case(stream_call::receive);
}

TEST(socket_syscall, sendmsg_refuses_ancillary_data) {
    int64_t fd = sys_socket(inet::AF_INET, inet::SOCK_DGRAM, 0, 0, 0, 0);
    ASSERT_TRUE(fd >= 0);

    user_page page;
    ASSERT_TRUE(page.ready());
    lay_out_message(page, 0, 4, 4);
    page.at<user_msghdr>(MSG_HDR)->controllen = 16;

    int64_t rc = 0;
    {
        user_space_scope scope(page.ctx);
        rc = sys_sendmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, 0, 0, 0, 0);
    }
    EXPECT_EQ(rc, syscall::EOPNOTSUPP);

    EXPECT_EQ(resource::close(sched::current(), static_cast<resource::handle_t>(fd)), resource::OK);
}

// Unix stream sends

constexpr uint64_t AF_UNIX = 1;

// Where the unix stream tests keep a pair's handles and the bytes they send
constexpr size_t UNIX_PAIR_AT = 1024;
constexpr size_t UNIX_SEND_AT = 2048;
constexpr size_t UNIX_SEND_MAX = pmm::PAGE_SIZE - UNIX_SEND_AT;

struct unix_pair {
    int32_t a;
    int32_t b;
};

// A connected pair made the way userland makes one, with both handles landing in the page
static bool make_unix_pair(user_page& page, unix_pair* out) {
    int64_t rc = 0;
    {
        user_space_scope scope(page.ctx);
        rc = sys_socketpair(AF_UNIX, inet::SOCK_STREAM, 0, page.addr + UNIX_PAIR_AT, 0, 0);
    }
    if (rc != 0) {
        return false;
    }

    out->a = page.at<int32_t>(UNIX_PAIR_AT)[0];
    out->b = page.at<int32_t>(UNIX_PAIR_AT)[1];
    return true;
}

static void close_unix_pair(sched::task* task, const unix_pair& pair) {
    (void)resource::close(task, pair.a);
    (void)resource::close(task, pair.b);
}

// Sends `len` bytes from the page, to the address at MSG_NAME when `addrlen` is set
static int64_t unix_send(user_page& page, int32_t fd, size_t len, uint64_t flags, uint64_t addrlen = 0) {
    user_space_scope scope(page.ctx);
    uint64_t addr = addrlen ? page.addr + MSG_NAME : 0;
    return sys_sendto(static_cast<uint64_t>(fd), page.addr + UNIX_SEND_AT, len, flags, addr, addrlen);
}

TEST(socket_syscall, a_unix_stream_send_reaches_the_peer) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hello", 5);
    EXPECT_EQ(unix_send(page, pair.a, 5, inet::MSG_NOSIGNAL), 5);

    char buf[8] = {};
    EXPECT_EQ(resource::read(task, pair.b, buf, sizeof(buf)), static_cast<ssize_t>(5));
    EXPECT_EQ(string::memcmp(buf, "hello", 5), 0);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_unix_stream_sendmsg_gathers_its_vector) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(MSG_BUF_A), "unix", 4);
    string::memcpy(page.at<char>(MSG_BUF_B), "pair", 4);
    lay_out_message(page, 0, 4, 4);

    int64_t sent = 0;
    {
        user_space_scope scope(page.ctx);
        sent = sys_sendmsg(static_cast<uint64_t>(pair.a), page.addr + MSG_HDR, 0, 0, 0, 0);
    }
    EXPECT_EQ(sent, static_cast<int64_t>(8));

    char buf[16] = {};
    EXPECT_EQ(resource::read(task, pair.b, buf, sizeof(buf)), static_cast<ssize_t>(8));
    EXPECT_EQ(string::memcmp(buf, "unixpair", 8), 0);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_unix_stream_send_refuses_what_a_stream_cannot_carry) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    EXPECT_EQ(unix_send(page, pair.a, 1, 0, 8), syscall::EISCONN);
    EXPECT_EQ(unix_send(page, pair.a, 1, inet::MSG_OOB), syscall::EOPNOTSUPP);

    int64_t lone = sys_socket(AF_UNIX, inet::SOCK_STREAM, 0, 0, 0, 0);
    ASSERT_TRUE(lone >= 0);
    EXPECT_EQ(unix_send(page, static_cast<int32_t>(lone), 1, 0), syscall::ENOTCONN);
    EXPECT_EQ(unix_send(page, static_cast<int32_t>(lone), 1, 0, 8), syscall::EOPNOTSUPP);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(lone)), resource::OK);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_nonblocking_unix_send_stops_when_the_stream_is_full) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    // The send that fills the stream takes only what fits instead of waiting
    int64_t last = 0;
    int64_t n = 0;
    while ((n = unix_send(page, pair.a, UNIX_SEND_MAX, inet::MSG_DONTWAIT)) > 0) {
        last = n;
    }

    EXPECT_EQ(n, syscall::EAGAIN);
    EXPECT_TRUE(last > 0 && last < static_cast<int64_t>(UNIX_SEND_MAX));

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_unix_stream_send_to_a_closed_peer_reports_epipe) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    EXPECT_EQ(resource::close(task, pair.b), resource::OK);
    EXPECT_EQ(unix_send(page, pair.a, 1, inet::MSG_NOSIGNAL), syscall::EPIPE);

    EXPECT_EQ(resource::close(task, pair.a), resource::OK);
}

TEST(socket_syscall, unix_stream_receive_calls_still_refuse) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    int64_t rc = 0;
    {
        user_space_scope scope(page.ctx);
        rc = sys_recvfrom(static_cast<uint64_t>(pair.b), page.addr + UNIX_SEND_AT, 16, 0, 0, 0);
    }
    EXPECT_EQ(rc, syscall::EOPNOTSUPP);

    close_unix_pair(task, pair);
}
