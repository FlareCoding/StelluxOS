#include "net/tcp.h"
#include "net/inet_socket.h"
#include "net/ipv4.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/net.h"
#include "common/logging.h"
#include "common/string.h"
#include "common/ring_buffer.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

// TCP port registry: global linked list of all TCP sockets that have
// a local port assigned (via bind). Used by tcp_recv to find the
// matching socket for incoming segments.
__PRIVILEGED_DATA static tcp_socket* g_tcp_sock_list = nullptr;
__PRIVILEGED_DATA static sync::spinlock g_tcp_sock_lock = sync::SPINLOCK_INIT;

// Simple ISN generator. A real implementation uses a clock-based scheme
// for security (RFC 6528), but a monotonic counter is correct for now.
__PRIVILEGED_DATA static volatile uint32_t g_tcp_isn_counter = 1000;

static uint32_t tcp_generate_isn() {
    return __atomic_fetch_add(&g_tcp_isn_counter, 64000, __ATOMIC_RELAXED);
}

constexpr uint16_t TCP_DEFAULT_WINDOW = 8192;
constexpr uint32_t TCP_DEFAULT_BACKLOG = 16;
constexpr uint32_t TCP_MAX_BACKLOG = 128;
constexpr size_t TCP_RX_BUF_CAPACITY = 16384;

// Send a TCP segment with the given parameters.
// Builds the 20-byte header, computes the checksum, and hands to ipv4_send.
__PRIVILEGED_CODE static int32_t tcp_send_segment(
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    uint32_t seq, uint32_t ack_num,
    uint8_t flags, uint16_t window,
    const uint8_t* data, size_t data_len
) {
    size_t seg_len = sizeof(tcp_header) + data_len;
    auto* buf = static_cast<uint8_t*>(heap::kzalloc(seg_len));
    if (!buf) {
        return ERR_NOMEM;
    }

    auto* hdr = reinterpret_cast<tcp_header*>(buf);
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->seq = htonl(seq);
    hdr->ack = htonl(ack_num);
    hdr->data_off = (5 << 4); // 5 * 4 = 20 bytes, no options
    hdr->flags = flags;
    hdr->window = htons(window);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data && data_len > 0) {
        string::memcpy(buf + sizeof(tcp_header), data, data_len);
    }

    uint16_t csum = tcp_checksum(htonl(src_ip), htonl(dst_ip), buf, seg_len);
    hdr->checksum = (csum == 0) ? static_cast<uint16_t>(0xFFFF) : csum;

    // TCP segments sent from the RX path (like SYN-ACK in response to SYN)
    // must use deferred TX to avoid re-entering ipv4_send/ARP from within
    // the NIC driver's receive context. Same mechanism as ICMP echo replies.
    netif* iface = get_default_netif();
    queue_deferred_tx(iface, dst_ip, IPV4_PROTO_TCP, buf, seg_len);
    heap::kfree(buf);
    return OK;
}

// Send a TCP segment directly via ipv4_send (for syscall context, not RX path).
// Used by tcp_write where deferred TX would not drain promptly.
__PRIVILEGED_CODE static int32_t tcp_send_data(
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    uint32_t seq, uint32_t ack_num,
    uint8_t flags, uint16_t window,
    const uint8_t* data, size_t data_len
) {
    size_t seg_len = sizeof(tcp_header) + data_len;
    auto* buf = static_cast<uint8_t*>(heap::kzalloc(seg_len));
    if (!buf) {
        return ERR_NOMEM;
    }

    auto* hdr = reinterpret_cast<tcp_header*>(buf);
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->seq = htonl(seq);
    hdr->ack = htonl(ack_num);
    hdr->data_off = (5 << 4);
    hdr->flags = flags;
    hdr->window = htons(window);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data && data_len > 0) {
        string::memcpy(buf + sizeof(tcp_header), data, data_len);
    }

    uint16_t csum = tcp_checksum(htonl(src_ip), htonl(dst_ip), buf, seg_len);
    hdr->checksum = (csum == 0) ? static_cast<uint16_t>(0xFFFF) : csum;

    netif* iface = get_default_netif();
    int32_t rc = ipv4_send(iface, dst_ip, IPV4_PROTO_TCP, buf, seg_len, src_ip);
    heap::kfree(buf);
    return rc;
}

// Destroy an orphaned tcp_socket (no resource_object attached).
// Called from tcp_recv when the state machine reaches CLOSED after
// the application has already called close().
static void tcp_destroy_socket(tcp_socket* sock) {
    if (!sock) return;

    if (sock->local_port != 0) {
        tcp_unregister_socket(sock);
    }

    if (sock->rx_buf) {
        ring_buffer_destroy(sock->rx_buf);
        sock->rx_buf = nullptr;
    }

    log::debug("tcp: socket destroyed :%u <-> %u.%u.%u.%u:%u",
              sock->local_port,
              (sock->remote_addr >> 24) & 0xFF,
              (sock->remote_addr >> 16) & 0xFF,
              (sock->remote_addr >> 8) & 0xFF,
              sock->remote_addr & 0xFF,
              sock->remote_port);

    heap::kfree_delete(sock);
}

__PRIVILEGED_CODE static void tcp_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    auto* sock = static_cast<tcp_socket*>(obj->impl);

    // Drain accept queue if this was a LISTEN socket
    if (sock->state == tcp_state::LISTEN) {
        while (tcp_pending_conn* pc = sock->accept_queue.pop_front()) {
            if (pc->conn_obj) {
                resource::resource_release(pc->conn_obj);
            }
            heap::kfree(pc);
        }
        sync::wake_all(sock->accept_wq);

        if (sock->local_port != 0) {
            tcp_unregister_socket(sock);
        }
        if (sock->rx_buf) {
            ring_buffer_destroy(sock->rx_buf);
        }
        heap::kfree_delete(sock);
        obj->impl = nullptr;
        return;
    }

    // For ESTABLISHED or CLOSE_WAIT: send FIN and let the socket linger
    // in the port registry until the FIN handshake completes.
    if (sock->state == tcp_state::ESTABLISHED) {
        uint32_t fin_seq = sock->snd_nxt;
        sock->snd_nxt++;
        sock->state = tcp_state::FIN_WAIT_1;
        obj->impl = nullptr;

        log::debug("tcp: close() -> FIN_WAIT_1 :%u <-> %u.%u.%u.%u:%u",
                  sock->local_port,
                  (sock->remote_addr >> 24) & 0xFF,
                  (sock->remote_addr >> 16) & 0xFF,
                  (sock->remote_addr >> 8) & 0xFF,
                  sock->remote_addr & 0xFF,
                  sock->remote_port);

        tcp_send_segment(
            sock->local_addr, sock->local_port,
            sock->remote_addr, sock->remote_port,
            fin_seq, sock->rcv_nxt,
            TCP_FIN | TCP_ACK, TCP_DEFAULT_WINDOW,
            nullptr, 0);
        return;
    }

    if (sock->state == tcp_state::CLOSE_WAIT) {
        uint32_t fin_seq = sock->snd_nxt;
        sock->snd_nxt++;
        sock->state = tcp_state::LAST_ACK;
        obj->impl = nullptr;

        log::debug("tcp: close() -> LAST_ACK :%u <-> %u.%u.%u.%u:%u",
                  sock->local_port,
                  (sock->remote_addr >> 24) & 0xFF,
                  (sock->remote_addr >> 16) & 0xFF,
                  (sock->remote_addr >> 8) & 0xFF,
                  sock->remote_addr & 0xFF,
                  sock->remote_port);

        tcp_send_segment(
            sock->local_addr, sock->local_port,
            sock->remote_addr, sock->remote_port,
            fin_seq, sock->rcv_nxt,
            TCP_FIN | TCP_ACK, TCP_DEFAULT_WINDOW,
            nullptr, 0);
        return;
    }

    // All other states (CLOSED, SYN_RECEIVED, etc.): immediate cleanup
    if (sock->local_port != 0) {
        tcp_unregister_socket(sock);
    }
    if (sock->rx_buf) {
        ring_buffer_destroy(sock->rx_buf);
    }
    heap::kfree_delete(sock);
    obj->impl = nullptr;
}

__PRIVILEGED_CODE static int32_t tcp_bind(
    resource::resource_object* obj, const void* kaddr, size_t addrlen
) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<tcp_socket*>(obj->impl);

    if (!kaddr || addrlen < sizeof(kernel_sockaddr_in)) {
        return resource::ERR_INVAL;
    }
    const auto* addr = static_cast<const kernel_sockaddr_in*>(kaddr);
    if (addr->sin_family != AF_INET_VAL) {
        return resource::ERR_INVAL;
    }

    uint32_t bind_addr = ntohl(addr->sin_addr);
    uint16_t bind_port = ntohs(addr->sin_port);

    if (bind_addr != 0 && !is_local_ip(bind_addr)) {
        return resource::ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);

    if (sock->local_port != 0) {
        sync::spin_unlock_irqrestore(sock->lock, irq);
        return resource::ERR_INVAL;
    }

    if (sock->state != tcp_state::CLOSED) {
        sync::spin_unlock_irqrestore(sock->lock, irq);
        return resource::ERR_INVAL;
    }

    sock->local_addr = bind_addr;
    sock->local_port = bind_port;

    if (!tcp_try_register(sock)) {
        sock->local_addr = 0;
        sock->local_port = 0;
        sync::spin_unlock_irqrestore(sock->lock, irq);
        return resource::ERR_ADDRINUSE;
    }

    sync::spin_unlock_irqrestore(sock->lock, irq);
    return resource::OK;
}

__PRIVILEGED_CODE static int32_t tcp_listen(
    resource::resource_object* obj, int32_t backlog
) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<tcp_socket*>(obj->impl);

    sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);

    if (sock->state != tcp_state::CLOSED || sock->local_port == 0) {
        sync::spin_unlock_irqrestore(sock->lock, irq);
        return resource::ERR_INVAL;
    }

    uint32_t bl = (backlog <= 0) ? TCP_DEFAULT_BACKLOG
                                 : static_cast<uint32_t>(backlog);
    if (bl > TCP_MAX_BACKLOG) {
        bl = TCP_MAX_BACKLOG;
    }

    sock->backlog = bl;
    sock->pending_count = 0;
    sock->accept_queue.init();
    sock->accept_wq.init();
    sock->state = tcp_state::LISTEN;

    sync::spin_unlock_irqrestore(sock->lock, irq);

    log::info("tcp: listening on port %u (backlog %u)", sock->local_port, bl);
    return resource::OK;
}

__PRIVILEGED_CODE static int32_t tcp_accept(
    resource::resource_object* obj, resource::resource_object** new_obj,
    void* kaddr, size_t* addrlen, bool nonblock
) {
    if (!obj || !obj->impl || !new_obj) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<tcp_socket*>(obj->impl);
    if (sock->state != tcp_state::LISTEN) {
        return resource::ERR_INVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return resource::ERR_IO;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);

    if (sock->accept_queue.empty()) {
        if (nonblock) {
            sync::spin_unlock_irqrestore(sock->lock, irq);
            return resource::ERR_AGAIN;
        }
    }
    while (sock->accept_queue.empty()
           && sock->state == tcp_state::LISTEN
           && !__atomic_load_n(&task->kill_pending, __ATOMIC_ACQUIRE)) {
        irq = sync::wait(sock->accept_wq, sock->lock, irq);
    }
    if (__atomic_load_n(&task->kill_pending, __ATOMIC_ACQUIRE)) {
        sync::spin_unlock_irqrestore(sock->lock, irq);
        return resource::ERR_INTR;
    }
    if (sock->accept_queue.empty()) {
        sync::spin_unlock_irqrestore(sock->lock, irq);
        return resource::ERR_INVAL;
    }

    tcp_pending_conn* pc = sock->accept_queue.pop_front();
    sock->pending_count--;
    sync::spin_unlock_irqrestore(sock->lock, irq);

    *new_obj = pc->conn_obj;
    auto* child = static_cast<tcp_socket*>(pc->conn_obj->impl);
    heap::kfree(pc);

    // Fill in peer address for the accept() syscall
    if (kaddr && addrlen && *addrlen >= sizeof(kernel_sockaddr_in)) {
        auto* peer = static_cast<kernel_sockaddr_in*>(kaddr);
        string::memset(peer, 0, sizeof(*peer));
        peer->sin_family = AF_INET_VAL;
        peer->sin_port = htons(child->remote_port);
        peer->sin_addr = htonl(child->remote_addr);
        *addrlen = sizeof(kernel_sockaddr_in);
    }

    return resource::OK;
}

__PRIVILEGED_CODE static ssize_t tcp_read(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !kdst) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<tcp_socket*>(obj->impl);
    if (sock->state != tcp_state::ESTABLISHED && sock->state != tcp_state::CLOSE_WAIT) {
        return resource::ERR_NOTCONN;
    }

    if (!sock->rx_buf) {
        return resource::ERR_IO;
    }

    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    return ring_buffer_read(sock->rx_buf, static_cast<uint8_t*>(kdst), count, nonblock);
}

__PRIVILEGED_CODE static ssize_t tcp_write(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags
) {
    (void)flags;
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<tcp_socket*>(obj->impl);
    if (sock->state != tcp_state::ESTABLISHED && sock->state != tcp_state::CLOSE_WAIT) {
        return resource::ERR_NOTCONN;
    }

    // Stamp with current snd_nxt, then advance
    sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
    uint32_t seq = sock->snd_nxt;
    uint32_t ack_val = sock->rcv_nxt;
    uint32_t src_ip = sock->local_addr;
    uint16_t src_port = sock->local_port;
    uint32_t dst_ip = sock->remote_addr;
    uint16_t dst_port = sock->remote_port;
    sock->snd_nxt += static_cast<uint32_t>(count);
    sync::spin_unlock_irqrestore(sock->lock, irq);

    int32_t rc = tcp_send_data(
        src_ip, src_port, dst_ip, dst_port,
        seq, ack_val,
        TCP_PSH | TCP_ACK, TCP_DEFAULT_WINDOW,
        static_cast<const uint8_t*>(ksrc), count);

    if (rc != OK) {
        return resource::ERR_IO;
    }
    return static_cast<ssize_t>(count);
}

static const resource::resource_ops g_tcp_ops = {
    tcp_read,
    tcp_write,
    tcp_close,
    nullptr, // ioctl
    nullptr, // mmap
    nullptr, // sendto
    nullptr, // recvfrom
    tcp_bind,
    tcp_listen,
    tcp_accept,
    nullptr, // connect
};

// Look up a TCP socket matching an incoming segment.
// For LISTEN sockets: match on local port (and optionally local addr).
// For established connections: match on the full 4-tuple.
static tcp_socket* tcp_lookup(uint32_t src_ip, uint16_t src_port,
                              uint32_t dst_ip, uint16_t dst_port) {
    tcp_socket* listen_match = nullptr;

    for (tcp_socket* s = g_tcp_sock_list; s; s = s->next) {
        if (s->local_port != dst_port) continue;
        if (s->local_addr != 0 && s->local_addr != dst_ip) continue;

        // Exact 4-tuple match (established connections) takes priority
        if (s->remote_port == src_port && s->remote_addr == src_ip) {
            return s;
        }

        // LISTEN socket (remote == 0) is a fallback match
        if (s->state == tcp_state::LISTEN && !listen_match) {
            listen_match = s;
        }
    }

    return listen_match;
}

// Handle a SYN arriving on a LISTEN socket (RFC 9293 Section 3.10.7.2)
static void tcp_listen_recv_syn(tcp_socket* listener,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port,
                                uint32_t their_seq) {
    sync::irq_state irq = sync::spin_lock_irqsave(listener->lock);
    if (listener->pending_count >= listener->backlog) {
        sync::spin_unlock_irqrestore(listener->lock, irq);
        log::debug("tcp: backlog full, dropping SYN");
        return;
    }
    sync::spin_unlock_irqrestore(listener->lock, irq);

    // Create child socket for this connection
    auto* child = heap::kalloc_new<tcp_socket>();
    if (!child) {
        return;
    }

    uint32_t our_isn = tcp_generate_isn();

    child->state = tcp_state::SYN_RECEIVED;
    child->local_addr = dst_ip;
    child->local_port = dst_port;
    child->remote_addr = src_ip;
    child->remote_port = src_port;
    child->rcv_nxt = their_seq + 1;
    child->snd_una = our_isn;
    child->snd_nxt = our_isn + 1;
    child->rx_buf = nullptr;
    child->rx_wq.init();
    child->parent = listener;
    child->backlog = 0;
    child->pending_count = 0;
    child->accept_queue.init();
    child->accept_wq.init();
    child->lock = sync::SPINLOCK_INIT;
    child->next = nullptr;

    // Register in the port registry so the ACK finds this socket by 4-tuple
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_tcp_sock_lock);
        child->next = g_tcp_sock_list;
        g_tcp_sock_list = child;
    });

    log::info("tcp: SYN on :%u from %u.%u.%u.%u:%u -> SYN_RECEIVED (isn=%u)",
              dst_port,
              (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
              (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port,
              our_isn);

    // Send SYN-ACK
    tcp_send_segment(dst_ip, dst_port, src_ip, src_port,
                     our_isn, child->rcv_nxt,
                     TCP_SYN | TCP_ACK, TCP_DEFAULT_WINDOW,
                     nullptr, 0);
}

// Handle an ACK arriving on a SYN_RECEIVED socket (completes handshake)
static void tcp_synrcvd_recv_ack(tcp_socket* sock, uint32_t ack_num) {
    sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);

    // Verify the ACK acknowledges our SYN
    if (ack_num != sock->snd_nxt) {
        sync::spin_unlock_irqrestore(sock->lock, irq);
        log::debug("tcp: bad ACK in SYN_RECEIVED (expected %u, got %u)",
                   sock->snd_nxt, ack_num);
        return;
    }

    sock->snd_una = ack_num;
    sock->state = tcp_state::ESTABLISHED;

    // Allocate receive buffer for data transfer
    if (!sock->rx_buf) {
        sock->rx_buf = ring_buffer_create(TCP_RX_BUF_CAPACITY);
    }

    sync::spin_unlock_irqrestore(sock->lock, irq);

    log::info("tcp: connection ESTABLISHED :%u <-> %u.%u.%u.%u:%u",
              sock->local_port,
              (sock->remote_addr >> 24) & 0xFF,
              (sock->remote_addr >> 16) & 0xFF,
              (sock->remote_addr >> 8) & 0xFF,
              sock->remote_addr & 0xFF,
              sock->remote_port);

    // Wrap in a resource_object and enqueue on parent's accept queue
    tcp_socket* parent = sock->parent;
    if (!parent || parent->state != tcp_state::LISTEN) {
        return;
    }

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        return;
    }
    obj->type = resource::resource_type::SOCKET;
    obj->ops = &g_tcp_ops;
    obj->impl = sock;

    auto* pc = static_cast<tcp_pending_conn*>(
        heap::kzalloc(sizeof(tcp_pending_conn)));
    if (!pc) {
        heap::kfree_delete(obj);
        return;
    }
    pc->conn_obj = obj;

    irq = sync::spin_lock_irqsave(parent->lock);
    parent->accept_queue.push_back(pc);
    parent->pending_count++;
    sync::spin_unlock_irqrestore(parent->lock, irq);
    sync::wake_one(parent->accept_wq);
}

} // anonymous namespace

bool tcp_try_register(tcp_socket* sock) {
    if (!sock || sock->local_port == 0) {
        return false;
    }
    bool registered = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_tcp_sock_lock);

        bool conflict = false;
        for (tcp_socket* s = g_tcp_sock_list; s; s = s->next) {
            if (s == sock) continue;
            if (s->local_port != sock->local_port) continue;
            if (sock->local_addr == 0 || s->local_addr == 0
                || s->local_addr == sock->local_addr) {
                conflict = true;
                break;
            }
        }

        if (!conflict) {
            sock->next = g_tcp_sock_list;
            g_tcp_sock_list = sock;
            registered = true;
        }
    });
    return registered;
}

void tcp_unregister_socket(tcp_socket* sock) {
    if (!sock) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_tcp_sock_lock);
        tcp_socket** pp = &g_tcp_sock_list;
        while (*pp) {
            if (*pp == sock) {
                *pp = sock->next;
                sock->next = nullptr;
                break;
            }
            pp = &(*pp)->next;
        }
    });
}

int32_t create_tcp_socket(resource::resource_object** out) {
    if (!out) {
        return resource::ERR_INVAL;
    }

    auto* sock = heap::kalloc_new<tcp_socket>();
    if (!sock) {
        return resource::ERR_NOMEM;
    }
    sock->state = tcp_state::CLOSED;
    sock->local_addr = 0;
    sock->local_port = 0;
    sock->remote_addr = 0;
    sock->remote_port = 0;
    sock->snd_una = 0;
    sock->snd_nxt = 0;
    sock->rcv_nxt = 0;
    sock->rx_buf = nullptr;
    sock->rx_wq.init();
    sock->parent = nullptr;
    sock->backlog = 0;
    sock->pending_count = 0;
    sock->accept_queue.init();
    sock->accept_wq.init();
    sock->lock = sync::SPINLOCK_INIT;
    sock->next = nullptr;

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::kfree_delete(sock);
        return resource::ERR_NOMEM;
    }
    obj->type = resource::resource_type::SOCKET;
    obj->ops = &g_tcp_ops;
    obj->impl = sock;

    *out = obj;
    return resource::OK;
}

void tcp_recv(netif* iface, uint32_t src_ip, uint32_t dst_ip,
              const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(tcp_header)) {
        return;
    }

    const auto* hdr = reinterpret_cast<const tcp_header*>(data);

    size_t hdr_len = tcp_header_len(hdr);
    if (hdr_len < sizeof(tcp_header) || hdr_len > len) {
        return;
    }

    // Verify checksum over pseudo-header + full TCP segment
    uint16_t computed = tcp_checksum(htonl(src_ip), htonl(dst_ip), data, len);
    if (computed != 0) {
        log::debug("tcp: bad checksum, dropping");
        return;
    }

    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack_num = ntohl(hdr->ack);
    uint8_t flags = hdr->flags;
    size_t payload_len = len - hdr_len;

    // Build a human-readable flag string for logging
    char flag_str[24];
    int pos = 0;
    if (flags & TCP_SYN) { flag_str[pos++] = 'S'; }
    if (flags & TCP_ACK) { flag_str[pos++] = 'A'; }
    if (flags & TCP_FIN) { flag_str[pos++] = 'F'; }
    if (flags & TCP_RST) { flag_str[pos++] = 'R'; }
    if (flags & TCP_PSH) { flag_str[pos++] = 'P'; }
    if (flags & TCP_URG) { flag_str[pos++] = 'U'; }
    if (pos == 0) { flag_str[pos++] = '-'; }
    flag_str[pos] = '\0';

    // Look up matching socket
    tcp_socket* sock = nullptr;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_tcp_sock_lock);
        sock = tcp_lookup(src_ip, src_port, dst_ip, dst_port);
    });

    if (!sock) {
        log::info("tcp: %u.%u.%u.%u:%u -> :%u [%s] seq=%u ack=%u len=%u (no socket)",
                  (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                  (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port,
                  dst_port, flag_str, seq, ack_num,
                  static_cast<uint32_t>(payload_len));
        return;
    }

    const char* state_str = "?";
    switch (sock->state) {
    case tcp_state::CLOSED:       state_str = "CLOSED"; break;
    case tcp_state::LISTEN:       state_str = "LISTEN"; break;
    case tcp_state::SYN_SENT:     state_str = "SYN_SENT"; break;
    case tcp_state::SYN_RECEIVED: state_str = "SYN_RCVD"; break;
    case tcp_state::ESTABLISHED:  state_str = "ESTAB"; break;
    case tcp_state::FIN_WAIT_1:   state_str = "FIN_W1"; break;
    case tcp_state::FIN_WAIT_2:   state_str = "FIN_W2"; break;
    case tcp_state::CLOSE_WAIT:   state_str = "CLS_WAIT"; break;
    case tcp_state::LAST_ACK:     state_str = "LAST_ACK"; break;
    case tcp_state::TIME_WAIT:    state_str = "TIME_WAIT"; break;
    case tcp_state::CLOSING:      state_str = "CLOSING"; break;
    }

    log::debug("tcp: %u.%u.%u.%u:%u -> :%u [%s] seq=%u ack=%u len=%u (%s)",
              (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
              (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port,
              dst_port, flag_str, seq, ack_num,
              static_cast<uint32_t>(payload_len), state_str);

    // State machine dispatch
    switch (sock->state) {
    case tcp_state::LISTEN:
        if (flags & TCP_SYN) {
            tcp_listen_recv_syn(sock, src_ip, src_port, dst_ip, dst_port, seq);
        }
        break;

    case tcp_state::SYN_RECEIVED:
        if (flags & TCP_ACK) {
            tcp_synrcvd_recv_ack(sock, ack_num);
        }
        break;

    case tcp_state::ESTABLISHED: {
        // RFC 9293 Section 3.10.7.4 - ESTABLISHED state processing

        // ACK processing
        if (flags & TCP_ACK) {
            sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
            if (ack_num > sock->snd_nxt) {
                // ACKs something not yet sent - drop
                sync::spin_unlock_irqrestore(sock->lock, irq);
                break;
            }
            if (ack_num > sock->snd_una && ack_num <= sock->snd_nxt) {
                sock->snd_una = ack_num;
            }
            sync::spin_unlock_irqrestore(sock->lock, irq);
        }

        // Process segment text (data)
        if (payload_len > 0 && sock->rx_buf) {
            if (seq != sock->rcv_nxt) {
                // Out of order - drop (no reassembly yet)
                break;
            }

            ssize_t written = ring_buffer_write(
                sock->rx_buf,
                data + hdr_len,
                payload_len, true);

            if (written > 0) {
                sock->rcv_nxt += static_cast<uint32_t>(written);
            }

            // Send ACK: <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
            tcp_send_segment(
                sock->local_addr, sock->local_port,
                sock->remote_addr, sock->remote_port,
                sock->snd_nxt, sock->rcv_nxt,
                TCP_ACK, TCP_DEFAULT_WINDOW,
                nullptr, 0);
        }

        // FIN processing (minimal - just ACK it)
        if (flags & TCP_FIN) {
            sock->rcv_nxt++;
            tcp_send_segment(
                sock->local_addr, sock->local_port,
                sock->remote_addr, sock->remote_port,
                sock->snd_nxt, sock->rcv_nxt,
                TCP_ACK, TCP_DEFAULT_WINDOW,
                nullptr, 0);

            sock->state = tcp_state::CLOSE_WAIT;
            if (sock->rx_buf) {
                ring_buffer_close_write(sock->rx_buf);
            }
            log::info("tcp: FIN received, -> CLOSE_WAIT :%u <-> %u.%u.%u.%u:%u",
                      sock->local_port,
                      (sock->remote_addr >> 24) & 0xFF,
                      (sock->remote_addr >> 16) & 0xFF,
                      (sock->remote_addr >> 8) & 0xFF,
                      sock->remote_addr & 0xFF,
                      sock->remote_port);
        }
        break;
    }

    case tcp_state::FIN_WAIT_1: {
        // We sent FIN, waiting for peer's ACK and/or FIN

        // ACK processing
        if (flags & TCP_ACK) {
            sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
            if (ack_num > sock->snd_una && ack_num <= sock->snd_nxt) {
                sock->snd_una = ack_num;
            }
            bool fin_acked = (sock->snd_una == sock->snd_nxt);
            sync::spin_unlock_irqrestore(sock->lock, irq);

            if (fin_acked && !(flags & TCP_FIN)) {
                sock->state = tcp_state::FIN_WAIT_2;
                log::info("tcp: -> FIN_WAIT_2 :%u", sock->local_port);
            }
        }

        // FIN processing
        if (flags & TCP_FIN) {
            sock->rcv_nxt++;
            tcp_send_segment(
                sock->local_addr, sock->local_port,
                sock->remote_addr, sock->remote_port,
                sock->snd_nxt, sock->rcv_nxt,
                TCP_ACK, TCP_DEFAULT_WINDOW,
                nullptr, 0);

            bool fin_acked = (sock->snd_una == sock->snd_nxt);
            if (fin_acked) {
                // Our FIN was ACKed (maybe in this segment) -> TIME_WAIT
                log::info("tcp: -> TIME_WAIT :%u", sock->local_port);
                tcp_destroy_socket(sock);
            } else {
                // Simultaneous close: peer sent FIN before ACKing ours
                sock->state = tcp_state::CLOSING;
                log::info("tcp: -> CLOSING :%u", sock->local_port);
            }
        }
        break;
    }

    case tcp_state::FIN_WAIT_2: {
        // Our FIN ACKed, waiting for peer's FIN

        // Can still receive data per RFC
        if (payload_len > 0 && sock->rx_buf) {
            if (seq == sock->rcv_nxt) {
                ssize_t written = ring_buffer_write(
                    sock->rx_buf, data + hdr_len, payload_len, true);
                if (written > 0) {
                    sock->rcv_nxt += static_cast<uint32_t>(written);
                }
                tcp_send_segment(
                    sock->local_addr, sock->local_port,
                    sock->remote_addr, sock->remote_port,
                    sock->snd_nxt, sock->rcv_nxt,
                    TCP_ACK, TCP_DEFAULT_WINDOW,
                    nullptr, 0);
            }
        }

        // FIN processing
        if (flags & TCP_FIN) {
            sock->rcv_nxt++;
            tcp_send_segment(
                sock->local_addr, sock->local_port,
                sock->remote_addr, sock->remote_port,
                sock->snd_nxt, sock->rcv_nxt,
                TCP_ACK, TCP_DEFAULT_WINDOW,
                nullptr, 0);

            // TIME_WAIT -> immediate CLOSED (simplified, no 2*MSL timer)
            log::info("tcp: -> TIME_WAIT -> CLOSED :%u", sock->local_port);
            tcp_destroy_socket(sock);
        }
        break;
    }

    case tcp_state::CLOSE_WAIT:
        // Peer sent FIN, we haven't closed yet. Process ACKs for data
        // we may have sent. No data to receive (FIN was already received).
        if (flags & TCP_ACK) {
            sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
            if (ack_num > sock->snd_una && ack_num <= sock->snd_nxt) {
                sock->snd_una = ack_num;
            }
            sync::spin_unlock_irqrestore(sock->lock, irq);
        }
        break;

    case tcp_state::LAST_ACK:
        // We sent FIN after CLOSE_WAIT, waiting for peer's ACK
        if (flags & TCP_ACK) {
            sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
            if (ack_num > sock->snd_una && ack_num <= sock->snd_nxt) {
                sock->snd_una = ack_num;
            }
            bool fin_acked = (sock->snd_una == sock->snd_nxt);
            sync::spin_unlock_irqrestore(sock->lock, irq);

            if (fin_acked) {
                log::info("tcp: LAST_ACK -> CLOSED :%u", sock->local_port);
                tcp_destroy_socket(sock);
            }
        }
        break;

    case tcp_state::CLOSING:
        // Simultaneous close: we sent FIN, peer sent FIN, waiting for ACK of our FIN
        if (flags & TCP_ACK) {
            sync::irq_state irq = sync::spin_lock_irqsave(sock->lock);
            if (ack_num > sock->snd_una && ack_num <= sock->snd_nxt) {
                sock->snd_una = ack_num;
            }
            bool fin_acked = (sock->snd_una == sock->snd_nxt);
            sync::spin_unlock_irqrestore(sock->lock, irq);

            if (fin_acked) {
                // TIME_WAIT -> immediate CLOSED (simplified)
                log::info("tcp: CLOSING -> CLOSED :%u", sock->local_port);
                tcp_destroy_socket(sock);
            }
        }
        break;

    case tcp_state::TIME_WAIT:
        // Retransmitted FIN from peer: re-ACK it
        if (flags & TCP_FIN) {
            tcp_send_segment(
                sock->local_addr, sock->local_port,
                sock->remote_addr, sock->remote_port,
                sock->snd_nxt, sock->rcv_nxt,
                TCP_ACK, TCP_DEFAULT_WINDOW,
                nullptr, 0);
        }
        break;

    default:
        break;
    }
}

} // namespace net
