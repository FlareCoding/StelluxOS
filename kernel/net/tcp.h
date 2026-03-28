#ifndef STELLUX_NET_TCP_H
#define STELLUX_NET_TCP_H

#include "net/net.h"
#include "resource/resource.h"
#include "rc/ref_counted.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "common/list.h"

struct ring_buffer;

namespace net {

// TCP header flags (RFC 9293 Section 3.1)
constexpr uint8_t TCP_FIN = 0x01; // no more data from sender
constexpr uint8_t TCP_SYN = 0x02; // synchronize sequence numbers
constexpr uint8_t TCP_RST = 0x04; // reset the connection
constexpr uint8_t TCP_PSH = 0x08; // push buffered data to receiver
constexpr uint8_t TCP_ACK = 0x10; // acknowledgment field is valid
constexpr uint8_t TCP_URG = 0x20; // urgent pointer field is valid

// TCP header (RFC 9293 Section 3.1) — minimum 20 bytes.
// All multi-byte fields are in network byte order on the wire.
struct tcp_header {
    uint16_t src_port;   // source port
    uint16_t dst_port;   // destination port
    uint32_t seq;        // sequence number
    uint32_t ack;        // acknowledgment number
    uint8_t  data_off;   // upper 4 bits: data offset (header length in 32-bit words)
    uint8_t  flags;      // lower 6 bits: URG|ACK|PSH|RST|SYN|FIN
    uint16_t window;     // receive window size
    uint16_t checksum;   // checksum (pseudo-header + header + data)
    uint16_t urgent_ptr; // urgent pointer (only valid if URG flag set)
} __attribute__((packed));

static_assert(sizeof(tcp_header) == 20, "tcp_header must be 20 bytes");

// Extract the header length in bytes from the data_off field.
// The upper 4 bits encode the number of 32-bit words in the header.
inline constexpr size_t tcp_header_len(const tcp_header* hdr) {
    return static_cast<size_t>((hdr->data_off >> 4) & 0xF) * 4;
}

// TCP connection states (RFC 9293 Section 3.3.2)
enum class tcp_state : uint8_t {
    CLOSED,
    LISTEN,
    SYN_SENT,
    SYN_RECEIVED,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    CLOSE_WAIT,
    LAST_ACK,
    TIME_WAIT,
    CLOSING,
};

// Entry in a LISTEN socket's accept queue.
struct tcp_pending_conn {
    list::node link;
    resource::resource_object* conn_obj;
};

// TCP socket: one per connection (or one per listening port)
struct tcp_socket : rc::ref_counted<tcp_socket> {
    tcp_state      state;

    // Local endpoint (set by bind or connect)
    uint32_t       local_addr;  // host byte order, 0 = any
    uint16_t       local_port;  // host byte order, 0 = unbound

    // Remote endpoint (set by connect or accept, 0 for LISTEN sockets)
    uint32_t       remote_addr; // host byte order
    uint16_t       remote_port; // host byte order

    // Sequence number tracking (RFC 9293 Section 3.3.1)
    uint32_t       snd_una;     // oldest unacknowledged seq (Send Unacknowledged)
    uint32_t       snd_nxt;     // next seq we will send (Send Next)
    uint32_t       rcv_nxt;     // next seq we expect to receive (Receive Next)

    // ESTABLISHED state: receive buffer for incoming data
    ring_buffer*   rx_buf;
    sync::wait_queue rx_wq;

    // LISTEN state: accept queue for completed connections
    tcp_socket*    parent;        // backpointer to LISTEN socket (for child sockets)
    uint32_t       backlog;       // max pending connections
    uint32_t       pending_count; // current pending connections
    list::head<tcp_pending_conn, &tcp_pending_conn::link> accept_queue;
    sync::wait_queue accept_wq;

    uint32_t       so_options;   // bitmask of socket options
    sync::spinlock lock;
    tcp_socket*    next;        // linked list for port registry

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(tcp_socket* self);
};

/**
 * Create an AF_INET SOCK_STREAM socket in CLOSED state.
 */
int32_t create_tcp_socket(resource::resource_object** out);

/**
 * Atomically check for binding conflicts and register if none found.
 * @return true if registered, false if conflict.
 */
bool tcp_try_register(tcp_socket* sock);

/**
 * Unregister a TCP socket from the port registry.
 * @return true if the socket was found and removed, false if not found.
 */
bool tcp_unregister_socket(tcp_socket* sock);

/**
 * Process a received TCP segment (after IPv4 header is stripped).
 * Validates header, verifies checksum, and dispatches to connection state.
 * @param src_ip Source IP in host byte order.
 * @param dst_ip Destination IP in host byte order.
 */
void tcp_recv(netif* iface, uint32_t src_ip, uint32_t dst_ip,
              const uint8_t* data, size_t len);

} // namespace net

#endif // STELLUX_NET_TCP_H
