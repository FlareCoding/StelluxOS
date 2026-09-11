#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "../net/stub_interface.h"
#include "syscall/handlers/sys_socket.h"
#include "resource/resource.h"
#include "net/inet.h"
#include "net/udp_socket.h"
#include "net/udp.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"

using test_helpers::user_space_scope;
using namespace net;

TEST_SUITE(socket_syscall);

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

// One eager user page, reachable from the kernel through its frame
struct user_page {
    mm::mm_context* ctx = nullptr;
    uintptr_t addr = 0;
    uint8_t* bytes = nullptr;

    user_page() {
        ctx = mm::mm_context_create();
        if (!ctx) {
            return;
        }

        uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
        uint32_t flags = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;
        if (mm::mm_context_map_anonymous(ctx, 0, pmm::PAGE_SIZE, prot, flags, &addr) != mm::MM_CTX_OK) {
            addr = 0;
            return;
        }

        pmm::phys_addr_t phys = paging::get_physical(addr, ctx->pt_root);
        bytes = phys ? static_cast<uint8_t*>(paging::phys_to_virt(phys)) : nullptr;
    }

    ~user_page() {
        if (ctx) {
            mm::mm_context_release(ctx);
        }
    }

    bool ready() const { return bytes != nullptr; }

    template <typename T>
    T* at(size_t offset) { return reinterpret_cast<T*>(bytes + offset); }
};

static uint32_t handle_flags_of(sched::task* task, int64_t h) {
    resource::resource_object* obj = nullptr;
    uint32_t flags = 0;
    if (resource::get_handle_object(task->handles, static_cast<resource::handle_t>(h),
                                    resource::RIGHT_READ, &obj, &flags) == resource::HANDLE_OK) {
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

TEST(socket_syscall, creation_flags_land_on_the_handle) {
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
