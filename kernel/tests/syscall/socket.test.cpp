#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "../net/stub_interface.h"
#include "syscall/handlers/sys_socket.h"
#include "syscall/handlers/sys_error_map.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_io.h"
#include "syscall/handlers/sys_shutdown.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/handlers/sys_proc.h"
#include "resource/resource.h"
#include "resource/socket_ops.h"
#include "socket/unix_socket.h"
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
#include "sync/mutex.h"
#include "sync/poll.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;
using test_helpers::spin_wait;
using test_helpers::unpin;
using test_helpers::blocks_before_deadline;
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
constexpr size_t MSG_CONTROL = 2560;

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

struct user_cmsghdr {
    uint64_t len;
    int32_t level;
    int32_t type;
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

TEST(socket_syscall, a_connect_to_a_listener_of_another_type_reports_eprototype) {
    EXPECT_EQ(syscall::error_map::map_socket_op_error(resource::ERR_PROTOTYPE), syscall::EPROTOTYPE);
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

// A UDP socket bound to TEST_PORT, nonblocking since the test runner can never wait
static int64_t make_bound_udp_socket(sched::task* task) {
    int64_t fd = sys_socket(inet::AF_INET, inet::SOCK_DGRAM | fs::O_NONBLOCK, 0, 0, 0, 0);
    if (fd < 0) {
        return fd;
    }

    udp::udp_socket* sock = udp_socket_of(task, fd);
    if (!sock || udp::socket_bind(sock, ipv4::UNSPECIFIED_ADDR, TEST_PORT) != OK) {
        (void)resource::close(task, static_cast<resource::handle_t>(fd));
        return -1;
    }

    return fd;
}

static bool deliver_datagram(uint16_t dst_port, const char* payload, size_t payload_len) {
    packet* pkt = make_datagram(dst_port, payload, payload_len);
    return pkt && udp::socket_deliver(pkt) == OK;
}

TEST(socket_syscall, a_readv_takes_exactly_one_datagram) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t fd = make_bound_udp_socket(task);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(deliver_datagram(TEST_PORT, "hello", 5));
    ASSERT_TRUE(deliver_datagram(TEST_PORT, "world", 5));

    user_page page;
    ASSERT_TRUE(page.ready());
    lay_out_message(page, 0, 3, 16);

    int64_t first = 0;
    {
        user_space_scope scope(page.ctx);
        first = sys_readv(static_cast<uint64_t>(fd), page.addr + MSG_IOVS, 3, 0, 0, 0);
    }

    EXPECT_EQ(first, static_cast<int64_t>(5));
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_A), "hel", 3), 0);
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_B), "lo", 2), 0);

    int64_t second = 0;
    {
        user_space_scope scope(page.ctx);
        second = sys_readv(static_cast<uint64_t>(fd), page.addr + MSG_IOVS, 3, 0, 0, 0);
    }

    EXPECT_EQ(second, static_cast<int64_t>(5));
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_A), "wor", 3), 0);
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_B), "ld", 2), 0);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(fd)), resource::OK);
}

TEST(socket_syscall, recvmsg_reports_a_datagram_that_lost_its_tail) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t fd = make_bound_udp_socket(task);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(deliver_datagram(TEST_PORT, "hello world", 11));
    ASSERT_TRUE(deliver_datagram(TEST_PORT, "hello world", 11));

    user_page page;
    ASSERT_TRUE(page.ready());
    lay_out_message(page, inet::SOCKADDR_IN_LEN, 2, 3);
    const user_msghdr* hdr = page.at<user_msghdr>(MSG_HDR);

    int64_t copied = 0;
    {
        user_space_scope scope(page.ctx);
        copied = sys_recvmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, 0, 0, 0, 0);
    }

    EXPECT_EQ(copied, static_cast<int64_t>(5));
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_A), "he", 2), 0);
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_B), "llo", 3), 0);
    EXPECT_EQ(hdr->flags, inet::MSG_TRUNC);

    // With MSG_TRUNC the receive reports the length the datagram had
    int64_t full_length = 0;
    {
        user_space_scope scope(page.ctx);
        full_length = sys_recvmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, inet::MSG_TRUNC, 0, 0, 0);
    }

    EXPECT_EQ(full_length, static_cast<int64_t>(11));
    EXPECT_EQ(hdr->flags, inet::MSG_TRUNC);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(fd)), resource::OK);
}

TEST(socket_syscall, a_zero_byte_receive_takes_a_datagram_that_a_zero_byte_read_leaves) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t fd = make_bound_udp_socket(task);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(deliver_datagram(TEST_PORT, "hello", 5));
    ASSERT_TRUE(deliver_datagram(TEST_PORT, "world!", 6));
    ASSERT_TRUE(deliver_datagram(TEST_PORT, "again", 5));

    user_page page;
    ASSERT_TRUE(page.ready());
    lay_out_message(page, inet::SOCKADDR_IN_LEN, 0, 0);
    uint64_t buf = page.addr + MSG_BUF_A;

    int64_t zero_byte_read = 0;
    int64_t zero_byte_recvfrom = 0;
    int64_t zero_byte_recvmsg = 0;
    int64_t next_receive = 0;
    {
        user_space_scope scope(page.ctx);
        zero_byte_read = sys_read(static_cast<uint64_t>(fd), buf, 0, 0, 0, 0);
        zero_byte_recvfrom = sys_recvfrom(static_cast<uint64_t>(fd), buf, 0, inet::MSG_TRUNC, 0, 0);
        zero_byte_recvmsg = sys_recvmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, 0, 0, 0, 0);
        next_receive = sys_recvfrom(static_cast<uint64_t>(fd), buf, 16, 0, 0, 0);
    }

    EXPECT_EQ(zero_byte_read, static_cast<int64_t>(0));
    EXPECT_EQ(zero_byte_recvfrom, static_cast<int64_t>(5));
    EXPECT_EQ(zero_byte_recvmsg, static_cast<int64_t>(0));
    EXPECT_EQ(page.at<user_msghdr>(MSG_HDR)->flags, inet::MSG_TRUNC);
    EXPECT_EQ(next_receive, static_cast<int64_t>(5));
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_A), "again", 5), 0);

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
    discard = 3,
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
                uint64_t flags = inet::MSG_WAITALL;
                if (run.call == stream_call::discard) {
                    flags |= inet::MSG_TRUNC;
                }

                run.result = sys_recvfrom(static_cast<uint64_t>(fd), run.buf, STREAM_BYTES, flags, 0, 0);
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

TEST(socket_syscall, a_zero_byte_discarding_receive_leaves_the_stream_untouched) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    loopback_listener listener(task);
    ASSERT_TRUE(listener.fd >= 0);
    loopback_client client(task);
    ASSERT_TRUE(client.ready());
    resource::resource_object* server = listener.accept();
    ASSERT_NOT_NULL(server);

    EXPECT_EQ(server->ops->socket->sendto(server, "hello", 5, inet::MSG_DONTWAIT, nullptr, 0), 5);
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (client.queued() < 5 && clock::now_ns() < deadline) {
    }

    ASSERT_EQ(client.queued(), 5u);

    uint32_t flags = inet::MSG_TRUNC | inet::MSG_DONTWAIT;
    EXPECT_EQ(sys_recvfrom(static_cast<uint64_t>(client.fd), 0, 0, flags, 0, 0), static_cast<int64_t>(0));
    EXPECT_EQ(client.queued(), 5u);

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

    bool client_sends = call == stream_call::send || call == stream_call::write;
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

    if (call == stream_call::receive) {
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

TEST(socket_syscall, a_stream_discard_with_waitall_drops_past_one_staging_round) {
    run_stream_case(stream_call::discard);
}

// Writes an SCM_RIGHTS message naming `count` handles at MSG_CONTROL, returning its length
static size_t write_rights_message(user_page& page, const int32_t* handles, size_t count) {
    auto* head = page.at<user_cmsghdr>(MSG_CONTROL);
    head->len = sizeof(user_cmsghdr) + count * sizeof(int32_t);
    head->level = inet::SOL_SOCKET;
    head->type = inet::SCM_RIGHTS;
    string::memcpy(head + 1, handles, count * sizeof(int32_t));

    return head->len;
}

// Points the message header at `len` bytes of control data at MSG_CONTROL
static void set_control(user_page& page, uint64_t len) {
    user_msghdr* hdr = page.at<user_msghdr>(MSG_HDR);
    hdr->control = page.addr + MSG_CONTROL;
    hdr->controllen = len;
}

static int64_t send_message(user_page& page, int64_t fd) {
    user_space_scope scope(page.ctx);
    return sys_sendmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, 0, 0, 0, 0);
}

static int64_t receive_message(user_page& page, int64_t fd, uint64_t flags) {
    user_space_scope scope(page.ctx);
    return sys_recvmsg(static_cast<uint64_t>(fd), page.addr + MSG_HDR, flags, 0, 0, 0);
}

TEST(socket_syscall, a_socket_that_cannot_pass_handles_ignores_them_and_refuses_other_control_data) {
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

    lay_out_message(page, inet::SOCKADDR_IN_LEN, 4, 4);
    *page.at<inet::sockaddr_in>(MSG_NAME) = {inet::AF_INET, htons(67), ipv4::BROADCAST_ADDR, {}};

    int32_t own_handle = static_cast<int32_t>(fd);
    set_control(page, write_rights_message(page, &own_handle, 1));
    EXPECT_EQ(send_message(page, fd), static_cast<int64_t>(8));
    EXPECT_EQ(link.frames_sent(), static_cast<size_t>(1));

    page.at<user_cmsghdr>(MSG_CONTROL)->level = static_cast<int32_t>(inet::IPPROTO_IP);
    EXPECT_EQ(send_message(page, fd), syscall::EOPNOTSUPP);
    EXPECT_EQ(link.frames_sent(), static_cast<size_t>(1));

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(fd)), resource::OK);
}

// Unix stream sends and receives

constexpr uint64_t AF_UNIX = 1;
constexpr uint64_t SOCK_SEQPACKET = 5;

// Where the unix stream tests keep a pair's handles and the bytes they receive, peek at and send
constexpr size_t UNIX_PAIR_AT = 1024;
constexpr size_t UNIX_RECV_AT = 1536;
constexpr size_t UNIX_PEEK_AT = 1792;
constexpr size_t UNIX_SEND_AT = 2048;
constexpr size_t UNIX_SEND_MAX = pmm::PAGE_SIZE - UNIX_SEND_AT;

struct unix_pair {
    int32_t a;
    int32_t b;
};

// A connected pair made the way userland makes one, with both handles landing in the page
static bool make_unix_pair(user_page& page, unix_pair* out, uint64_t type = inet::SOCK_STREAM) {
    int64_t rc = 0;
    {
        user_space_scope scope(page.ctx);
        rc = sys_socketpair(AF_UNIX, type, 0, page.addr + UNIX_PAIR_AT, 0, 0);
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

static int64_t unix_receive(user_page& page, int32_t fd, size_t len, uint64_t flags) {
    user_space_scope scope(page.ctx);
    return sys_recvfrom(static_cast<uint64_t>(fd), page.addr + UNIX_RECV_AT, len, flags, 0, 0);
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

TEST(socket_syscall, a_unix_stream_receive_takes_what_the_peer_sent) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hello", 5);
    ASSERT_EQ(unix_send(page, pair.a, 5, 0), static_cast<int64_t>(5));
    EXPECT_EQ(unix_receive(page, pair.b, 16, 0), static_cast<int64_t>(5));
    EXPECT_EQ(string::memcmp(page.at<char>(UNIX_RECV_AT), "hello", 5), 0);
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), syscall::EAGAIN);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_zero_byte_unix_stream_receive_leaves_the_bytes_queued) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hello", 5);
    ASSERT_EQ(unix_send(page, pair.a, 5, 0), static_cast<int64_t>(5));
    EXPECT_EQ(unix_receive(page, pair.b, 0, inet::MSG_DONTWAIT), static_cast<int64_t>(0));

    lay_out_message(page, 0, 0, 0);
    EXPECT_EQ(receive_message(page, pair.b, inet::MSG_DONTWAIT), static_cast<int64_t>(0));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(5));

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_unix_stream_peek_leaves_the_bytes_queued) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "abc", 3);
    ASSERT_EQ(unix_send(page, pair.a, 3, 0), static_cast<int64_t>(3));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_PEEK), static_cast<int64_t>(3));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(3));
    EXPECT_EQ(string::memcmp(page.at<char>(UNIX_RECV_AT), "abc", 3), 0);
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), syscall::EAGAIN);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_truncating_unix_stream_receive_discards_the_bytes_unless_it_peeks) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "abcdef", 6);
    ASSERT_EQ(unix_send(page, pair.a, 6, 0), static_cast<int64_t>(6));
    EXPECT_EQ(unix_receive(page, pair.b, 4, inet::MSG_TRUNC | inet::MSG_PEEK), static_cast<int64_t>(4));
    EXPECT_EQ(unix_receive(page, pair.b, 4, inet::MSG_TRUNC), static_cast<int64_t>(4));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(2));
    EXPECT_EQ(string::memcmp(page.at<char>(UNIX_RECV_AT), "ef", 2), 0);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_unix_stream_recvmsg_scatters_across_its_vector) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "unixpair", 8);
    ASSERT_EQ(unix_send(page, pair.a, 8, 0), static_cast<int64_t>(8));
    lay_out_message(page, 16, 4, 4);

    int64_t received = 0;
    {
        user_space_scope scope(page.ctx);
        received = sys_recvmsg(static_cast<uint64_t>(pair.b), page.addr + MSG_HDR, inet::MSG_DONTWAIT, 0, 0, 0);
    }
    EXPECT_EQ(received, static_cast<int64_t>(8));
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_A), "unix", 4), 0);
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_B), "pair", 4), 0);
    EXPECT_EQ(page.at<user_msghdr>(MSG_HDR)->namelen, 0u);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_unix_stream_recvmsg_drains_what_a_closed_peer_left) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "unixpair", 8);
    ASSERT_EQ(unix_send(page, pair.a, 8, 0), static_cast<int64_t>(8));
    EXPECT_EQ(resource::close(task, pair.a), resource::OK);

    // A closed peer ends the stream after what it sent instead of cutting a receive short
    lay_out_message(page, 0, 4, 4);
    int64_t received = 0;
    {
        user_space_scope scope(page.ctx);
        received = sys_recvmsg(static_cast<uint64_t>(pair.b), page.addr + MSG_HDR, inet::MSG_DONTWAIT, 0, 0, 0);
    }
    EXPECT_EQ(received, static_cast<int64_t>(8));
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_B), "pair", 4), 0);
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(0));

    EXPECT_EQ(resource::close(task, pair.b), resource::OK);
}

// Handle passing

constexpr size_t PIPE_FDS_AT = 1040;
constexpr uint64_t CONTROL_ROOM = 64;
constexpr uint64_t CMSG_ALIGNMENT = 8;
constexpr uint64_t MAX_CONTROL_BYTES = 20480;
constexpr uint64_t ONE_HANDLE_LEN = sizeof(user_cmsghdr) + sizeof(int32_t);
constexpr uint64_t ONE_HANDLE_SPACE = (ONE_HANDLE_LEN + CMSG_ALIGNMENT - 1) & ~(CMSG_ALIGNMENT - 1);

struct pipe_ends {
    int32_t read;
    int32_t write;
};

static bool make_pipe(user_page& page, pipe_ends* out, uint32_t flags = 0) {
    int64_t rc = 0;
    {
        user_space_scope scope(page.ctx);
        rc = sys_pipe2(page.addr + PIPE_FDS_AT, flags, 0, 0, 0, 0);
    }
    if (rc != 0) {
        return false;
    }

    out->read = page.at<int32_t>(PIPE_FDS_AT)[0];
    out->write = page.at<int32_t>(PIPE_FDS_AT)[1];

    return true;
}

static void close_pipe(sched::task* task, const pipe_ends& pipe) {
    (void)resource::close(task, pipe.read);
    (void)resource::close(task, pipe.write);
}

// The object behind an open handle, for comparing identities and counting references
static resource::resource_object* object_of(sched::task* task, int32_t handle) {
    resource::resource_object* obj = nullptr;
    if (resource::get_handle_object(task->handles, handle, 0, &obj) != resource::HANDLE_OK) {
        return nullptr;
    }

    resource::resource_release(obj);

    return obj;
}

// Sends one byte with an SCM_RIGHTS message naming `count` handles
static int64_t send_with_rights(user_page& page, int32_t fd, const int32_t* handles, size_t count) {
    *page.at<char>(MSG_BUF_A) = 'x';
    lay_out_message(page, 0, 1, 0);
    set_control(page, write_rights_message(page, handles, count));

    return send_message(page, fd);
}

// Receives without waiting into a zeroed control buffer of `room` bytes, or none when `room` is 0
static int64_t receive_with_control(user_page& page, int32_t fd, uint64_t room, uint64_t flags) {
    lay_out_message(page, 0, 4, 4);
    string::memset(page.at<char>(MSG_CONTROL), 0, CONTROL_ROOM);
    if (room > 0) {
        set_control(page, room);
    }

    return receive_message(page, fd, flags | inet::MSG_DONTWAIT);
}

// The first handle the SCM_RIGHTS message at MSG_CONTROL names
static int32_t first_received_handle(user_page& page) {
    return *reinterpret_cast<int32_t*>(page.at<user_cmsghdr>(MSG_CONTROL) + 1);
}

TEST(socket_syscall, sendmsg_passes_a_handle_that_recvmsg_installs) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe, fs::O_NONBLOCK | fs::O_CLOEXEC));

    resource::resource_object* reader = object_of(task, pipe.read);
    ASSERT_NOT_NULL(reader);

    EXPECT_EQ(send_with_rights(page, pair.a, &pipe.read, 1), static_cast<int64_t>(1));
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), static_cast<int64_t>(1));

    const user_msghdr* hdr = page.at<user_msghdr>(MSG_HDR);
    EXPECT_EQ(hdr->controllen, ONE_HANDLE_SPACE);
    EXPECT_EQ(hdr->flags, 0u);

    const user_cmsghdr* head = page.at<user_cmsghdr>(MSG_CONTROL);
    EXPECT_EQ(head->len, ONE_HANDLE_LEN);
    EXPECT_EQ(head->level, inet::SOL_SOCKET);
    EXPECT_EQ(head->type, inet::SCM_RIGHTS);

    // The received handle shares the object's status flags but not the sender's close-on-exec
    int32_t received = first_received_handle(page);
    EXPECT_EQ(object_of(task, received), reader);
    EXPECT_EQ(handle_flags_of(task, received), fs::O_NONBLOCK);

    char buf[4] = {};
    EXPECT_EQ(resource::write(task, pipe.write, "hi", 2), static_cast<ssize_t>(2));
    EXPECT_EQ(resource::read(task, received, buf, sizeof(buf)), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(buf, "hi", 2), 0);

    (void)resource::close(task, received);
    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, recvmsg_drops_the_handles_it_has_no_room_for) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    resource::resource_object* reader = object_of(task, pipe.read);
    ASSERT_NOT_NULL(reader);
    uint32_t refs_before = reader->ref_count();

    int32_t three_handles[3] = {pipe.read, pipe.read, pipe.read};
    EXPECT_EQ(send_with_rights(page, pair.a, three_handles, 3), static_cast<int64_t>(1));
    EXPECT_EQ(send_with_rights(page, pair.a, three_handles, 1), static_cast<int64_t>(1));

    // Room for one handle installs the first and drops the rest
    const user_msghdr* hdr = page.at<user_msghdr>(MSG_HDR);
    EXPECT_EQ(receive_with_control(page, pair.b, ONE_HANDLE_LEN, 0), static_cast<int64_t>(1));
    EXPECT_EQ(hdr->flags, inet::MSG_CTRUNC);
    EXPECT_EQ(hdr->controllen, ONE_HANDLE_LEN);

    int32_t installed = first_received_handle(page);
    EXPECT_EQ(object_of(task, installed), reader);

    // No room at all drops every handle
    EXPECT_EQ(receive_with_control(page, pair.b, 0, 0), static_cast<int64_t>(1));
    EXPECT_EQ(hdr->flags, inet::MSG_CTRUNC);
    EXPECT_EQ(hdr->controllen, 0u);

    // Only the installed handle kept a reference
    EXPECT_EQ(reader->ref_count(), refs_before + 1);

    (void)resource::close(task, installed);
    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

// Handles filling the rest of the runner's table
static resource::handle_t g_fillers[resource::DEFAULT_HANDLE_LIMIT];

TEST(socket_syscall, recvmsg_drops_the_handles_its_table_has_no_room_for) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    resource::resource_object* reader = object_of(task, pipe.read);
    ASSERT_NOT_NULL(reader);

    int32_t two_handles[2] = {pipe.read, pipe.read};
    EXPECT_EQ(send_with_rights(page, pair.a, two_handles, 2), static_cast<int64_t>(1));

    // Fill every free slot below the descriptor limit, then free one so only the first handle fits
    size_t fillers = 0;
    while (fillers < resource::DEFAULT_HANDLE_LIMIT) {
        int32_t rc = resource::alloc_task_handle(task, reader, reader->type, resource::RIGHT_READ, &g_fillers[fillers]);
        if (rc != resource::HANDLE_OK) {
            break;
        }

        fillers++;
    }

    ASSERT_TRUE(fillers > 0);
    fillers--;
    (void)resource::close(task, g_fillers[fillers]);

    const user_msghdr* hdr = page.at<user_msghdr>(MSG_HDR);
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), static_cast<int64_t>(1));
    EXPECT_EQ(hdr->flags, inet::MSG_CTRUNC);
    EXPECT_EQ(hdr->controllen, ONE_HANDLE_SPACE);

    (void)resource::close(task, first_received_handle(page));
    for (size_t i = 0; i < fillers; i++) {
        (void)resource::close(task, g_fillers[i]);
    }

    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

// One handle named as many times as a single send may carry
static int32_t g_full_batch[resource::MAX_PASSED_HANDLES];

static int64_t send_full_batch(user_page& page, int32_t fd, int32_t handle) {
    for (size_t i = 0; i < resource::MAX_PASSED_HANDLES; i++) {
        g_full_batch[i] = handle;
    }

    return send_with_rights(page, fd, g_full_batch, resource::MAX_PASSED_HANDLES);
}

TEST(socket_syscall, a_process_keeps_at_most_its_handle_limit_in_flight) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    // As many full batches as fit under the limit go out, and the next one is refused
    size_t batches_under_limit = resource::DEFAULT_HANDLE_LIMIT / resource::MAX_PASSED_HANDLES;
    for (size_t i = 0; i < batches_under_limit; i++) {
        ASSERT_EQ(send_full_batch(page, pair.a, pipe.read), static_cast<int64_t>(1));
    }

    EXPECT_EQ(send_full_batch(page, pair.a, pipe.read), syscall::ETOOMANYREFS);

    // Taking one batch refunds it, even when the receiver has no room for its handles
    EXPECT_EQ(receive_with_control(page, pair.b, 0, 0), static_cast<int64_t>(1));
    EXPECT_EQ(send_full_batch(page, pair.a, pipe.read), static_cast<int64_t>(1));

    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, closing_a_socket_refunds_the_handles_queued_for_it) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair first;
    ASSERT_TRUE(make_unix_pair(page, &first));
    unix_pair second;
    ASSERT_TRUE(make_unix_pair(page, &second));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    size_t batches_under_limit = resource::DEFAULT_HANDLE_LIMIT / resource::MAX_PASSED_HANDLES;
    for (size_t i = 0; i < batches_under_limit; i++) {
        ASSERT_EQ(send_full_batch(page, first.a, pipe.read), static_cast<int64_t>(1));
    }

    EXPECT_EQ(send_full_batch(page, second.a, pipe.read), syscall::ETOOMANYREFS);

    close_unix_pair(task, first);
    EXPECT_EQ(send_full_batch(page, second.a, pipe.read), static_cast<int64_t>(1));

    close_pipe(task, pipe);
    close_unix_pair(task, second);
}

TEST(socket_syscall, a_peek_charges_nothing_for_the_copies_it_installs) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    size_t batches_under_limit = resource::DEFAULT_HANDLE_LIMIT / resource::MAX_PASSED_HANDLES;
    for (size_t i = 0; i < batches_under_limit - 1; i++) {
        ASSERT_EQ(send_full_batch(page, pair.a, pipe.read), static_cast<int64_t>(1));
    }

    EXPECT_EQ(receive_with_control(page, pair.b, ONE_HANDLE_SPACE, inet::MSG_PEEK), static_cast<int64_t>(1));
    int32_t copy = first_received_handle(page);

    // The peeked batch stays queued and charged once, so exactly one more full batch fits
    EXPECT_EQ(send_full_batch(page, pair.a, pipe.read), static_cast<int64_t>(1));
    EXPECT_EQ(send_full_batch(page, pair.a, pipe.read), syscall::ETOOMANYREFS);

    (void)resource::close(task, copy);
    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_sendmsg_that_would_queue_a_process_holding_the_reader_reports_eloop) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    resource::resource_object* process = test_helpers::create_unstarted_process_object("/bin/hello");
    ASSERT_NOT_NULL(process);
    int32_t process_handle = -1;
    ASSERT_EQ(resource::alloc_task_handle(task, process, process->type, 0, &process_handle), resource::HANDLE_OK);
    resource::resource_release(process);

    // The process holds the reading end, so it may go the other way only
    int64_t rc = sys_proc_set_handle(static_cast<uint64_t>(process_handle), 3, static_cast<uint64_t>(pair.b), 0, 0, 0);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(send_with_rights(page, pair.a, &process_handle, 1), syscall::ELOOP);
    EXPECT_EQ(send_with_rights(page, pair.b, &process_handle, 1), static_cast<int64_t>(1));

    (void)resource::close(task, process_handle);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, sendmsg_passes_a_unix_socket_that_recvmsg_installs) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair carrier;
    ASSERT_TRUE(make_unix_pair(page, &carrier));
    unix_pair cargo;
    ASSERT_TRUE(make_unix_pair(page, &cargo, SOCK_SEQPACKET));

    EXPECT_EQ(send_with_rights(page, carrier.a, &cargo.a, 1), static_cast<int64_t>(1));
    EXPECT_EQ(receive_with_control(page, carrier.b, CONTROL_ROOM, 0), static_cast<int64_t>(1));

    // The installed handle is a second handle to the passed socket, usable like the first
    int32_t received = first_received_handle(page);
    EXPECT_EQ(object_of(task, received), object_of(task, cargo.a));

    char buf[4] = {};
    EXPECT_EQ(resource::write(task, received, "hi", 2), static_cast<ssize_t>(2));
    EXPECT_EQ(resource::read(task, cargo.b, buf, sizeof(buf)), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(buf, "hi", 2), 0);

    (void)resource::close(task, received);
    close_unix_pair(task, cargo);
    close_unix_pair(task, carrier);
}

TEST(socket_syscall, a_sendmsg_of_the_reading_socket_into_its_own_queue_reports_eloop) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    EXPECT_EQ(send_with_rights(page, pair.a, &pair.b, 1), syscall::ELOOP);
    EXPECT_EQ(send_with_rights(page, pair.a, &pair.a, 1), static_cast<int64_t>(1));

    close_unix_pair(task, pair);
}

TEST(socket_syscall, recvmsg_marks_the_handles_it_installs_close_on_exec_when_asked) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    EXPECT_EQ(send_with_rights(page, pair.a, &pipe.read, 1), static_cast<int64_t>(1));
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, inet::MSG_CMSG_CLOEXEC), static_cast<int64_t>(1));

    int32_t received = first_received_handle(page);
    EXPECT_EQ(handle_flags_of(task, received), resource::RESOURCE_HANDLE_CLOEXEC);

    (void)resource::close(task, received);
    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

// A copy of the page's message header on a page of its own that the receive can read but not write
static uintptr_t read_only_header(user_page& page) {
    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    uint32_t flags = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;
    if (mm::mm_context_map_anonymous(page.ctx, 0, pmm::PAGE_SIZE, prot, flags, &addr) != mm::MM_CTX_OK) {
        return 0;
    }

    pmm::phys_addr_t phys = paging::get_physical(addr, page.ctx->pt_root);
    if (!phys) {
        return 0;
    }

    string::memcpy(paging::phys_to_virt(phys), page.at<user_msghdr>(MSG_HDR), sizeof(user_msghdr));

    return mm::mm_context_mprotect(page.ctx, addr, pmm::PAGE_SIZE, mm::MM_PROT_READ) == mm::MM_CTX_OK ? addr : 0;
}

TEST(socket_syscall, a_recvmsg_that_cannot_write_its_header_or_control_leaves_the_message_queued) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    resource::resource_object* reader = object_of(task, pipe.read);
    ASSERT_NOT_NULL(reader);
    uint32_t refs_before = reader->ref_count();

    EXPECT_EQ(send_with_rights(page, pair.a, &pipe.read, 1), static_cast<int64_t>(1));

    lay_out_message(page, 0, 4, 4);
    set_control(page, CONTROL_ROOM);
    uintptr_t read_only = read_only_header(page);
    ASSERT_TRUE(read_only != 0);

    int64_t header_rc = 0;
    {
        user_space_scope scope(page.ctx);
        header_rc = sys_recvmsg(static_cast<uint64_t>(pair.b), read_only, inet::MSG_DONTWAIT, 0, 0, 0);
    }

    // The same read-only page as the control buffer
    page.at<user_msghdr>(MSG_HDR)->control = read_only;
    int64_t control_rc = receive_message(page, pair.b, inet::MSG_DONTWAIT);

    EXPECT_EQ(header_rc, syscall::EFAULT);
    EXPECT_EQ(control_rc, syscall::EFAULT);
    EXPECT_EQ(reader->ref_count(), refs_before + 1);

    // A receive that can report takes the byte and the handle it still finds queued
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), static_cast<int64_t>(1));
    int32_t received = first_received_handle(page);
    EXPECT_EQ(object_of(task, received), reader);

    (void)resource::close(task, received);
    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_peek_installs_copies_and_leaves_the_handles_queued) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    resource::resource_object* writer = object_of(task, pipe.write);
    ASSERT_NOT_NULL(writer);

    EXPECT_EQ(send_with_rights(page, pair.a, &pipe.write, 1), static_cast<int64_t>(1));

    // A peek without a control buffer reports the handles it could not install
    EXPECT_EQ(receive_with_control(page, pair.b, 0, inet::MSG_PEEK), static_cast<int64_t>(1));
    EXPECT_EQ(page.at<user_msghdr>(MSG_HDR)->flags, inet::MSG_CTRUNC);

    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, inet::MSG_PEEK), static_cast<int64_t>(1));
    int32_t copy = first_received_handle(page);

    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), static_cast<int64_t>(1));
    int32_t original = first_received_handle(page);

    EXPECT_NE(copy, original);
    EXPECT_EQ(object_of(task, copy), writer);
    EXPECT_EQ(object_of(task, original), writer);

    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), syscall::EAGAIN);

    (void)resource::close(task, copy);
    (void)resource::close(task, original);
    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, sendmsg_refuses_control_data_it_cannot_carry) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    resource::resource_object* reader = object_of(task, pipe.read);
    ASSERT_NOT_NULL(reader);
    uint32_t refs_before = reader->ref_count();

    *page.at<char>(MSG_BUF_A) = 'x';
    lay_out_message(page, 0, 1, 0);
    user_cmsghdr* head = page.at<user_cmsghdr>(MSG_CONTROL);

    set_control(page, write_rights_message(page, &pipe.read, 1));
    head->len = sizeof(user_cmsghdr) - 1;
    EXPECT_EQ(send_message(page, pair.a), syscall::EINVAL);

    set_control(page, write_rights_message(page, &pipe.read, 1));
    head->type = inet::SCM_CREDENTIALS;
    EXPECT_EQ(send_message(page, pair.a), syscall::EINVAL);

    int32_t stale = static_cast<int32_t>(sys_socket(inet::AF_INET, inet::SOCK_DGRAM, 0, 0, 0, 0));
    ASSERT_TRUE(stale >= 0);
    EXPECT_EQ(resource::close(task, stale), resource::OK);

    set_control(page, write_rights_message(page, &stale, 1));
    EXPECT_EQ(send_message(page, pair.a), syscall::EBADF);

    int32_t too_many_handles[resource::MAX_PASSED_HANDLES + 1];
    for (int32_t& handle : too_many_handles) {
        handle = pipe.read;
    }

    set_control(page, write_rights_message(page, too_many_handles, resource::MAX_PASSED_HANDLES + 1));
    EXPECT_EQ(send_message(page, pair.a), syscall::EINVAL);

    set_control(page, MAX_CONTROL_BYTES + 1);
    EXPECT_EQ(send_message(page, pair.a), syscall::ENOBUFS);

    // Messages for other protocols are skipped and the byte still goes out
    set_control(page, write_rights_message(page, &pipe.read, 1));
    head->level = static_cast<int32_t>(inet::IPPROTO_IP);
    EXPECT_EQ(send_message(page, pair.a), static_cast<int64_t>(1));

    // Nothing refused was queued or kept a reference
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), static_cast<int64_t>(1));
    EXPECT_EQ(page.at<user_msghdr>(MSG_HDR)->controllen, 0u);
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), syscall::EAGAIN);
    EXPECT_EQ(reader->ref_count(), refs_before);

    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_zero_byte_sendmsg_drops_the_handles_it_names) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    resource::resource_object* reader = object_of(task, pipe.read);
    ASSERT_NOT_NULL(reader);
    uint32_t refs_before = reader->ref_count();

    lay_out_message(page, 0, 0, 0);
    set_control(page, write_rights_message(page, &pipe.read, 1));

    EXPECT_EQ(send_message(page, pair.a), static_cast<int64_t>(0));
    EXPECT_EQ(reader->ref_count(), refs_before);
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), syscall::EAGAIN);

    // A send of no bytes still reports a socket with no connection
    int64_t lone = sys_socket(AF_UNIX, inet::SOCK_STREAM, 0, 0, 0, 0);
    ASSERT_TRUE(lone >= 0);

    EXPECT_EQ(send_message(page, lone), syscall::ENOTCONN);
    EXPECT_EQ(reader->ref_count(), refs_before);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(lone)), resource::OK);
    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_zero_byte_sendto_reaches_the_socket) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    EXPECT_EQ(unix_send(page, pair.a, 0, 0), static_cast<int64_t>(0));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), syscall::EAGAIN);

    // A send of no bytes needs no buffer, and still reports a socket with no connection
    int64_t lone = sys_socket(AF_UNIX, inet::SOCK_STREAM, 0, 0, 0, 0);
    ASSERT_TRUE(lone >= 0);
    EXPECT_EQ(sys_sendto(static_cast<uint64_t>(lone), 0, 0, 0, 0, 0), syscall::ENOTCONN);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(lone)), resource::OK);
    close_unix_pair(task, pair);
}

// Seqpacket pairs

TEST(socket_syscall, a_seqpacket_pair_keeps_each_message_whole) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "AB", 2);
    ASSERT_EQ(unix_send(page, pair.a, 2, 0), static_cast<int64_t>(2));
    string::memcpy(page.at<char>(UNIX_SEND_AT), "CDE", 3);
    ASSERT_EQ(unix_send(page, pair.a, 3, 0), static_cast<int64_t>(3));

    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(2));
    EXPECT_EQ(string::memcmp(page.at<char>(UNIX_RECV_AT), "AB", 2), 0);
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(3));
    EXPECT_EQ(string::memcmp(page.at<char>(UNIX_RECV_AT), "CDE", 3), 0);
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), syscall::EAGAIN);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_seqpacket_receive_that_does_not_fit_drops_the_rest_of_the_message) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hello world", 11);
    ASSERT_EQ(unix_send(page, pair.a, 11, 0), static_cast<int64_t>(11));
    ASSERT_EQ(unix_send(page, pair.a, 5, 0), static_cast<int64_t>(5));

    lay_out_message(page, 0, 2, 3);
    EXPECT_EQ(receive_message(page, pair.b, inet::MSG_DONTWAIT), static_cast<int64_t>(5));
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_A), "he", 2), 0);
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_B), "llo", 3), 0);
    EXPECT_EQ(page.at<user_msghdr>(MSG_HDR)->flags, inet::MSG_TRUNC);

    // The next receive starts at the next message rather than at the dropped tail
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(5));
    EXPECT_EQ(string::memcmp(page.at<char>(UNIX_RECV_AT), "hello", 5), 0);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_seqpacket_peek_leaves_the_message_queued) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hello", 5);
    ASSERT_EQ(unix_send(page, pair.a, 5, 0), static_cast<int64_t>(5));

    uint64_t peek = inet::MSG_PEEK | inet::MSG_DONTWAIT;
    EXPECT_EQ(unix_receive(page, pair.b, 2, peek | inet::MSG_TRUNC), static_cast<int64_t>(5));
    EXPECT_EQ(unix_receive(page, pair.b, 16, peek), static_cast<int64_t>(5));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(5));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), syscall::EAGAIN);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_seqpacket_writev_sends_one_message_that_one_readv_takes) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    string::memcpy(page.at<char>(MSG_BUF_A), "unix", 4);
    string::memcpy(page.at<char>(MSG_BUF_B), "pair", 4);
    lay_out_message(page, 0, 4, 4);

    int64_t first_write = 0;
    int64_t second_write = 0;
    {
        user_space_scope scope(page.ctx);
        first_write = sys_writev(static_cast<uint64_t>(pair.a), page.addr + MSG_IOVS, 3, 0, 0, 0);
        second_write = sys_writev(static_cast<uint64_t>(pair.a), page.addr + MSG_IOVS, 3, 0, 0, 0);
    }

    EXPECT_EQ(first_write, static_cast<int64_t>(8));
    EXPECT_EQ(second_write, static_cast<int64_t>(8));

    lay_out_message(page, 0, 3, 16);
    int64_t read_back = 0;
    {
        user_space_scope scope(page.ctx);
        read_back = sys_readv(static_cast<uint64_t>(pair.b), page.addr + MSG_IOVS, 3, 0, 0, 0);
    }

    EXPECT_EQ(read_back, static_cast<int64_t>(8));
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_A), "uni", 3), 0);
    EXPECT_EQ(string::memcmp(page.at<char>(MSG_BUF_B), "xpair", 5), 0);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_seqpacket_message_carries_the_handles_sent_with_it) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    pipe_ends pipe;
    ASSERT_TRUE(make_pipe(page, &pipe));

    resource::resource_object* reader = object_of(task, pipe.read);
    ASSERT_NOT_NULL(reader);

    EXPECT_EQ(send_with_rights(page, pair.a, &pipe.read, 1), static_cast<int64_t>(1));
    string::memcpy(page.at<char>(UNIX_SEND_AT), "y", 1);
    EXPECT_EQ(unix_send(page, pair.a, 1, 0), static_cast<int64_t>(1));

    // A peek installs copies of the handles and leaves the message queued
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, inet::MSG_PEEK), static_cast<int64_t>(1));
    int32_t peeked = first_received_handle(page);
    EXPECT_EQ(object_of(task, peeked), reader);

    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), static_cast<int64_t>(1));
    int32_t received = first_received_handle(page);
    EXPECT_EQ(object_of(task, received), reader);

    // The next message was sent without handles, so neither a peek nor a receive finds any
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, inet::MSG_PEEK), static_cast<int64_t>(1));
    EXPECT_EQ(page.at<user_msghdr>(MSG_HDR)->controllen, static_cast<uint64_t>(0));
    EXPECT_EQ(receive_with_control(page, pair.b, CONTROL_ROOM, 0), static_cast<int64_t>(1));
    EXPECT_EQ(page.at<user_msghdr>(MSG_HDR)->controllen, static_cast<uint64_t>(0));
    EXPECT_EQ(*page.at<char>(MSG_BUF_A), 'y');

    (void)resource::close(task, peeked);
    (void)resource::close(task, received);
    close_pipe(task, pipe);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_seqpacket_message_can_fill_the_whole_buffer_but_no_more) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    // The page repeated as many times as the largest message takes, with the last piece cut short
    size_t pieces = (socket::SEQPACKET_MAX_MESSAGE + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;
    user_iovec* iovs = page.at<user_iovec>(UNIX_SEND_AT);
    for (size_t i = 0; i < pieces; i++) {
        iovs[i] = {page.addr, pmm::PAGE_SIZE};
    }

    iovs[pieces - 1].len = socket::SEQPACKET_MAX_MESSAGE - (pieces - 1) * pmm::PAGE_SIZE;

    int64_t largest = 0;
    {
        user_space_scope scope(page.ctx);
        largest = sys_writev(static_cast<uint64_t>(pair.a), page.addr + UNIX_SEND_AT, pieces, 0, 0, 0);
    }

    EXPECT_EQ(largest, static_cast<int64_t>(socket::SEQPACKET_MAX_MESSAGE));

    uint64_t length_only = inet::MSG_TRUNC | inet::MSG_DONTWAIT;
    EXPECT_EQ(unix_receive(page, pair.b, 16, length_only), static_cast<int64_t>(socket::SEQPACKET_MAX_MESSAGE));
    EXPECT_EQ(unix_send(page, pair.a, socket::SEQPACKET_MAX_MESSAGE + 1, 0), syscall::EMSGSIZE);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_zero_byte_seqpacket_send_queues_no_message) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    EXPECT_EQ(unix_send(page, pair.a, 0, 0), static_cast<int64_t>(0));

    lay_out_message(page, 0, 0, 0);
    EXPECT_EQ(send_message(page, pair.a), static_cast<int64_t>(0));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), syscall::EAGAIN);

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_seqpacket_direction_queues_at_most_its_message_limit) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    resource::resource_object* sender = object_of(task, pair.a);
    ASSERT_NOT_NULL(sender);

    *page.at<char>(UNIX_SEND_AT) = 'm';
    size_t queued = 0;
    while (queued < socket::SEQPACKET_MAX_QUEUED_MESSAGES &&
           unix_send(page, pair.a, 1, inet::MSG_DONTWAIT) == static_cast<int64_t>(1)) {
        queued++;
    }

    EXPECT_EQ(queued, socket::SEQPACKET_MAX_QUEUED_MESSAGES);
    EXPECT_EQ(unix_send(page, pair.a, 1, inet::MSG_DONTWAIT), syscall::EAGAIN);
    EXPECT_EQ(sender->ops->poll(sender, nullptr) & sync::POLL_OUT, 0u);

    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(1));
    EXPECT_NE(sender->ops->poll(sender, nullptr) & sync::POLL_OUT, 0u);
    EXPECT_EQ(unix_send(page, pair.a, 1, inet::MSG_DONTWAIT), static_cast<int64_t>(1));

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_seqpacket_peer_that_closes_ends_the_messages) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair, SOCK_SEQPACKET));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "bye", 3);
    ASSERT_EQ(unix_send(page, pair.a, 3, 0), static_cast<int64_t>(3));
    EXPECT_EQ(resource::close(task, pair.a), resource::OK);

    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(3));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(0));
    EXPECT_EQ(unix_send(page, pair.b, 3, inet::MSG_NOSIGNAL), syscall::EPIPE);

    EXPECT_EQ(resource::close(task, pair.b), resource::OK);
}

TEST(socket_syscall, socket_makes_a_unix_seqpacket_socket) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t fd = sys_socket(AF_UNIX, SOCK_SEQPACKET, 0, 0, 0, 0);
    ASSERT_TRUE(fd >= 0);

    resource::resource_object* obj = object_of(task, static_cast<int32_t>(fd));
    ASSERT_NOT_NULL(obj);
    EXPECT_EQ(resource::socket_ops_of(obj)->max_message, socket::SEQPACKET_MAX_MESSAGE);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(fd)), resource::OK);
}

// A receive that waits runs in an elevated task of its own, through a handle of
// its own, since the runner is the idle task and cannot block
struct unix_receive_run {
    mm::mm_context*            ctx;
    uintptr_t                  buf;
    resource::resource_object* reader;
    size_t                     len;
    uint64_t                   flags;
    int64_t                    result;
    sync::atomic<uint32_t>     done;
};

static unix_receive_run g_unix_receive;
static unix_receive_run g_unix_peek;

static void run_unix_receive(void* arg) {
    unix_receive_run& run = *static_cast<unix_receive_run*>(arg);
    sched::task* self = sched::current();
    resource::handle_t h = -1;
    run.result = syscall::EBADF;

    if (resource::alloc_handle(self->handles, run.reader, resource::resource_type::SOCKET,
                               resource::RIGHT_READ, &h) == resource::HANDLE_OK) {
        {
            user_space_scope scope(run.ctx);
            run.result = sys_recvfrom(static_cast<uint64_t>(h), run.buf, run.len, run.flags, 0, 0);
        }

        (void)resource::close(self, h);
    }

    run.done.store_release(1);
    sched::exit(0);
}

static void prepare_unix_receive(user_page& page, resource::resource_object* reader, size_t len, uint64_t flags,
                                 unix_receive_run& run = g_unix_receive, size_t at = UNIX_RECV_AT) {
    run.ctx = page.ctx;
    run.buf = page.addr + at;
    run.reader = reader;
    run.len = len;
    run.flags = flags;
    run.result = 0;
    run.done.store_relaxed(0);
}

static sched::task* start_unix_receive(unix_receive_run& run = g_unix_receive) {
    return test_helpers::start_pinned_task(run_unix_receive, &run, "unix_receive");
}

static bool unix_stream_drains(resource::resource_object* reader) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    uint8_t probe = 0;
    while (clock::now_ns() < deadline) {
        if (reader->ops->socket->recvfrom(reader, &probe, 1, inet::MSG_PEEK | inet::MSG_DONTWAIT,
                                          nullptr, nullptr) == resource::ERR_AGAIN) {
            return true;
        }
    }

    return false;
}

// The second half goes out only after the call has taken the first, so it must wait for it
static void expect_waitall_across_two_sends(uint64_t flags) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    resource::resource_object* reader = nullptr;
    ASSERT_EQ(resource::get_handle_object(task->handles, pair.b, resource::RIGHT_READ, &reader),
              resource::HANDLE_OK);

    prepare_unix_receive(page, reader, 6, flags);
    sched::task* t = start_unix_receive();
    ASSERT_NOT_NULL(t);

    string::memcpy(page.at<char>(UNIX_SEND_AT), "abc", 3);
    EXPECT_EQ(unix_send(page, pair.a, 3, 0), static_cast<int64_t>(3));
    EXPECT_TRUE(unix_stream_drains(reader));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "def", 3);
    EXPECT_EQ(unix_send(page, pair.a, 3, 0), static_cast<int64_t>(3));

    EXPECT_TRUE(spin_wait(g_unix_receive.done));
    EXPECT_EQ(g_unix_receive.result, static_cast<int64_t>(6));
    EXPECT_TRUE(unix_stream_drains(reader));
    if (!(flags & inet::MSG_TRUNC)) {
        EXPECT_EQ(string::memcmp(page.at<char>(UNIX_RECV_AT), "abcdef", 6), 0);
    }

    unpin(t);
    resource::resource_release(reader);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_waitall_unix_stream_receive_waits_for_every_byte) {
    expect_waitall_across_two_sends(inet::MSG_WAITALL);
}

TEST(socket_syscall, a_waitall_unix_stream_discard_drops_every_byte) {
    expect_waitall_across_two_sends(inet::MSG_WAITALL | inet::MSG_TRUNC);
}

TEST(socket_syscall, a_waiting_unix_stream_receive_leaves_the_receive_lock_free) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    resource::resource_object* reader = nullptr;
    ASSERT_EQ(resource::get_handle_object(task->handles, pair.b, resource::RIGHT_READ, &reader),
              resource::HANDLE_OK);
    sync::mutex* lock = reader->ops->socket->receive_lock(reader);
    ASSERT_NOT_NULL(lock);

    prepare_unix_receive(page, reader, 16, 0);
    sched::task* t = start_unix_receive();
    ASSERT_NOT_NULL(t);

    // Asleep until bytes arrive, the receive holds up no other receiver
    EXPECT_TRUE(blocks_before_deadline(t));
    EXPECT_FALSE(sync::mutex_is_locked(*lock));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hi", 2);
    EXPECT_EQ(unix_send(page, pair.a, 2, 0), static_cast<int64_t>(2));
    EXPECT_TRUE(spin_wait(g_unix_receive.done));
    EXPECT_EQ(g_unix_receive.result, static_cast<int64_t>(2));

    unpin(t);
    resource::resource_release(reader);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_waiting_unix_stream_peek_leaves_the_wakeup_to_other_receivers) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    resource::resource_object* reader = nullptr;
    ASSERT_EQ(resource::get_handle_object(task->handles, pair.b, resource::RIGHT_READ, &reader),
              resource::HANDLE_OK);

    // A peek falls asleep first and a receive behind it, and the send must still reach the receive
    prepare_unix_receive(page, reader, 16, inet::MSG_PEEK, g_unix_peek, UNIX_PEEK_AT);
    sched::task* peeker = start_unix_receive(g_unix_peek);
    ASSERT_NOT_NULL(peeker);
    EXPECT_TRUE(blocks_before_deadline(peeker));

    prepare_unix_receive(page, reader, 16, 0);
    sched::task* receiver = start_unix_receive();
    ASSERT_NOT_NULL(receiver);
    EXPECT_TRUE(blocks_before_deadline(receiver));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hi", 2);
    EXPECT_EQ(unix_send(page, pair.a, 2, 0), static_cast<int64_t>(2));
    EXPECT_TRUE(spin_wait(g_unix_receive.done));
    EXPECT_EQ(g_unix_receive.result, static_cast<int64_t>(2));

    // Closing the sender ends the stream for the peek, whether or not it saw the bytes first
    EXPECT_EQ(resource::close(task, pair.a), resource::OK);
    EXPECT_TRUE(spin_wait(g_unix_peek.done));
    EXPECT_TRUE(g_unix_peek.result == 2 || g_unix_peek.result == 0);

    unpin(receiver);
    unpin(peeker);
    resource::resource_release(reader);
    EXPECT_EQ(resource::close(task, pair.b), resource::OK);
}

// Two receivers drain one stream at once, one through read and one through recv
constexpr size_t SHARED_STREAM_BYTES = 65536;
constexpr size_t SHARED_READ_SIZE    = 97;

struct shared_read_run {
    mm::mm_context*            ctx;
    uintptr_t                  buf;
    const uint8_t*             view;
    resource::resource_object* reader;
    bool                       use_read;
    uint64_t                   total;
    uint64_t                   sum;
    int64_t                    last;
    sync::atomic<uint32_t>     done;
};

static shared_read_run g_shared_reads[2];

static void run_shared_read(void* arg) {
    shared_read_run& run = *static_cast<shared_read_run*>(arg);
    sched::task* self = sched::current();
    resource::handle_t h = -1;
    run.last = syscall::EBADF;

    if (resource::alloc_handle(self->handles, run.reader, resource::resource_type::SOCKET,
                               resource::RIGHT_READ, &h) == resource::HANDLE_OK) {
        {
            user_space_scope scope(run.ctx);
            int64_t n = 1;
            while (n > 0) {
                n = run.use_read ? sys_read(static_cast<uint64_t>(h), run.buf, SHARED_READ_SIZE, 0, 0, 0)
                                 : sys_recvfrom(static_cast<uint64_t>(h), run.buf, SHARED_READ_SIZE, 0, 0, 0);
                for (int64_t i = 0; i < n; i++) {
                    run.sum += run.view[i];
                }

                run.total += n > 0 ? static_cast<uint64_t>(n) : 0;
            }

            run.last = n;
        }

        (void)resource::close(self, h);
    }

    run.done.store_release(1);
    sched::exit(0);
}

static void start_shared_reads(user_page& page, resource::resource_object* reader) {
    for (uint32_t i = 0; i < 2; i++) {
        shared_read_run& run = g_shared_reads[i];
        size_t at = i == 0 ? MSG_BUF_A : MSG_BUF_B;
        run.ctx = page.ctx;
        run.buf = page.addr + at;
        run.view = page.at<uint8_t>(at);
        run.reader = reader;
        run.use_read = i == 0;
        run.total = 0;
        run.sum = 0;
        run.last = 0;
        run.done.store_relaxed(0);
        RUN_ELEVATED({
            sched::task* t = sched::create_kernel_task(run_shared_read, &run, "shared_read", sched::TASK_FLAG_ELEVATED);
            if (t) {
                sched::enqueue(t);
            }
        });
    }
}

// Both receivers reach the end of the stream, having taken every byte between them exactly once
static void expect_shared_reads_took(size_t bytes, uint64_t sum) {
    EXPECT_TRUE(spin_wait(g_shared_reads[0].done));
    EXPECT_TRUE(spin_wait(g_shared_reads[1].done));
    EXPECT_EQ(g_shared_reads[0].total + g_shared_reads[1].total, static_cast<uint64_t>(bytes));
    EXPECT_EQ(g_shared_reads[0].sum + g_shared_reads[1].sum, sum);
    EXPECT_EQ(g_shared_reads[0].last, static_cast<int64_t>(0));
    EXPECT_EQ(g_shared_reads[1].last, static_cast<int64_t>(0));
}

TEST(socket_syscall, concurrent_unix_stream_receivers_take_each_byte_exactly_once) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    resource::resource_object* writer = nullptr;
    resource::resource_object* reader = nullptr;
    ASSERT_EQ(resource::get_handle_object(task->handles, pair.a, resource::RIGHT_WRITE, &writer),
              resource::HANDLE_OK);
    ASSERT_EQ(resource::get_handle_object(task->handles, pair.b, resource::RIGHT_READ, &reader),
              resource::HANDLE_OK);
    start_shared_reads(page, reader);

    // The runner cannot block, so it feeds the stream without waiting for room
    uint8_t piece[512];
    size_t sent = 0;
    uint64_t sent_sum = 0;
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (sent < SHARED_STREAM_BYTES && clock::now_ns() < deadline) {
        size_t len = SHARED_STREAM_BYTES - sent < sizeof(piece) ? SHARED_STREAM_BYTES - sent : sizeof(piece);
        for (size_t i = 0; i < len; i++) {
            piece[i] = stream_pattern(sent + i);
        }

        ssize_t n = writer->ops->socket->sendto(writer, piece, len, inet::MSG_DONTWAIT, nullptr, 0);
        for (ssize_t i = 0; i < n; i++) {
            sent_sum += piece[i];
        }

        sent += n > 0 ? static_cast<size_t>(n) : 0;
    }
    EXPECT_EQ(sent, SHARED_STREAM_BYTES);

    // Closing the writing end ends the stream for both receivers
    resource::resource_release(writer);
    EXPECT_EQ(resource::close(task, pair.a), resource::OK);
    expect_shared_reads_took(SHARED_STREAM_BYTES, sent_sum);

    resource::resource_release(reader);
    EXPECT_EQ(resource::close(task, pair.b), resource::OK);
}

// Unix stream shutdown

static int64_t unix_shut_down(int32_t fd, uint64_t how) {
    return sys_shutdown(static_cast<uint64_t>(fd), how, 0, 0, 0, 0);
}

TEST(socket_syscall, shutting_a_unix_stream_for_writing_ends_the_peers_stream) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hi", 2);
    ASSERT_EQ(unix_send(page, pair.a, 2, 0), static_cast<int64_t>(2));
    EXPECT_EQ(unix_shut_down(pair.a, resource::SHUT_WR), static_cast<int64_t>(0));

    // The peer drains what was sent before the end, and this end may send no more
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(2));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(0));
    EXPECT_EQ(unix_send(page, pair.a, 2, inet::MSG_NOSIGNAL), syscall::EPIPE);

    // The other direction stays open
    EXPECT_EQ(unix_send(page, pair.b, 2, 0), static_cast<int64_t>(2));
    EXPECT_EQ(unix_receive(page, pair.a, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(2));

    close_unix_pair(task, pair);
}

TEST(socket_syscall, shutting_a_unix_stream_for_reading_refuses_the_peers_sends) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    string::memcpy(page.at<char>(UNIX_SEND_AT), "hi", 2);
    ASSERT_EQ(unix_send(page, pair.b, 2, 0), static_cast<int64_t>(2));
    EXPECT_EQ(unix_shut_down(pair.a, resource::SHUT_RD), static_cast<int64_t>(0));

    // What arrived before the shutdown still reads, then the stream ends
    EXPECT_EQ(unix_receive(page, pair.a, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(2));
    EXPECT_EQ(unix_receive(page, pair.a, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(0));
    EXPECT_EQ(unix_send(page, pair.b, 2, inet::MSG_NOSIGNAL), syscall::EPIPE);

    // The other direction stays open
    EXPECT_EQ(unix_send(page, pair.a, 2, 0), static_cast<int64_t>(2));
    EXPECT_EQ(unix_receive(page, pair.b, 16, inet::MSG_DONTWAIT), static_cast<int64_t>(2));

    close_unix_pair(task, pair);
}

TEST(socket_syscall, a_unix_stream_shutdown_needs_a_connection_and_a_direction) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));
    EXPECT_EQ(unix_shut_down(pair.a, 3), syscall::EINVAL);

    int64_t lone = sys_socket(AF_UNIX, inet::SOCK_STREAM, 0, 0, 0, 0);
    ASSERT_TRUE(lone >= 0);
    EXPECT_EQ(unix_shut_down(static_cast<int32_t>(lone), resource::SHUT_RDWR), syscall::ENOTCONN);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(lone)), resource::OK);
    close_unix_pair(task, pair);
}

TEST(socket_syscall, shutting_a_unix_stream_for_reading_wakes_a_waiting_receive) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());
    unix_pair pair;
    ASSERT_TRUE(make_unix_pair(page, &pair));

    resource::resource_object* reader = nullptr;
    ASSERT_EQ(resource::get_handle_object(task->handles, pair.b, resource::RIGHT_READ, &reader),
              resource::HANDLE_OK);

    prepare_unix_receive(page, reader, 16, 0);
    sched::task* t = start_unix_receive();
    ASSERT_NOT_NULL(t);

    EXPECT_TRUE(blocks_before_deadline(t));
    EXPECT_EQ(unix_shut_down(pair.b, resource::SHUT_RD), static_cast<int64_t>(0));
    EXPECT_TRUE(spin_wait(g_unix_receive.done));
    EXPECT_EQ(g_unix_receive.result, static_cast<int64_t>(0));

    unpin(t);
    resource::resource_release(reader);
    close_unix_pair(task, pair);
}

// Concurrent TCP receives

TEST(socket_syscall, concurrent_tcp_stream_receivers_take_each_byte_exactly_once) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_region region;
    ASSERT_TRUE(region.ready());
    user_page page;
    ASSERT_TRUE(page.ready());
    loopback_listener listener(task);
    ASSERT_TRUE(listener.fd >= 0);

    uint64_t sent_sum = 0;
    for (size_t i = 0; i < STREAM_BYTES; i++) {
        region.byte(i) = stream_pattern(i);
        sent_sum += region.byte(i);
    }

    inet::sockaddr_in addr = {inet::AF_INET, htons(STREAM_PORT), g_loopback, {}};
    string::memcpy(&region.byte(STREAM_BYTES), &addr, sizeof(addr));

    g_stream_client.ctx = region.ctx;
    g_stream_client.buf = region.addr;
    g_stream_client.addr = region.addr + STREAM_BYTES;
    g_stream_client.call = stream_call::send;
    g_stream_client.result = 0;
    g_stream_client.done.store_relaxed(0);
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(run_stream_client, nullptr, "stream_client",
                                                   sched::TASK_FLAG_ELEVATED);
        if (t) {
            sched::enqueue(t);
        }
    });

    resource::resource_object* server = listener.accept();
    EXPECT_TRUE(server != nullptr);
    if (server) {
        start_shared_reads(page, server);
    }

    EXPECT_TRUE(spin_wait(g_stream_client.done));
    EXPECT_EQ(g_stream_client.result, static_cast<int64_t>(STREAM_BYTES));
    if (server) {
        expect_shared_reads_took(STREAM_BYTES, sent_sum);
        resource::resource_release(server);
    }
}
