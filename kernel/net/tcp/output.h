#ifndef STELLUX_NET_TCP_OUTPUT_H
#define STELLUX_NET_TCP_OUTPUT_H

#include "net/tcp/conn.h"
#include "net/tcp/wire.h"

namespace net {
namespace tcp {

/**
 * What a connection's control segments are built from, copied out under the
 * record lock so nothing is sent while it is held.
 */
struct segment_source {
    interface* iface;
    tuple      key;
    uint32_t   iss;
    uint32_t   snd_nxt;
    uint32_t   rcv_nxt;
    uint32_t   rcv_wnd;
    uint8_t    rcv_wscale;
    bool       wscale_ok;
    bool       sack_ok;
    bool       ts_ok;
    uint32_t   ts_offset;
    uint32_t   ts_recent;
};

/**
 * @brief The largest segment `iface` can carry behind the two headers (RFC 9293 3.7.1).
 */
uint16_t local_mss(const interface* iface);

/**
 * @brief The window field for `window` bytes at shift `wscale`, capped at the
 * sixteen bits the header holds.
 */
uint16_t window_field(uint32_t window, uint8_t wscale);

/**
 * @brief Copies out of `conn` what its segments are built from. Caller holds the lock.
 */
segment_source snapshot_source(const tcp_conn* conn);

/**
 * @brief Sends a segment without payload for `key` through `iface`, from the
 * local address of the key to its remote one, carrying `flags`, `seq`, `ack`,
 * `window` as it goes on the wire, and the options `opts` describes.
 * @return OK, or the error the route or the IPv4 layer reported.
 */
int32_t send_segment(interface* iface, const tuple& key, uint8_t flags, uint32_t seq, uint32_t ack,
                     uint16_t window, const tcp_options& opts);

/**
 * @brief Sends the SYN of an active open with every option this host offers.
 */
int32_t send_syn(const segment_source& src);

/**
 * @brief Sends the SYN-ACK of a connection that met a SYN, offering back what
 * the peer offered.
 */
int32_t send_syn_ack(const segment_source& src);

/**
 * @brief Sends a segment without payload at `snd_nxt` carrying `flags`, the
 * scaled receive window, and timestamps when they were negotiated.
 */
int32_t send_control(const segment_source& src, uint8_t flags);

/**
 * @brief Sends the FIN, which sits one before `snd_nxt` once counted, with
 * the acknowledgment, window, and timestamps of a control segment.
 */
int32_t send_fin(const segment_source& src);

/**
 * @brief Builds a segment carrying `len` queued bytes from `seq`, PSH when
 * `push`. Caller holds the lock. nullptr when no packet is free.
 */
packet* build_data_segment(const tcp_conn* conn, uint32_t seq, size_t len, bool push);

/**
 * @brief Checksums, counts, and hands a built segment to IPv4. Consumes it.
 */
int32_t transmit_segment(packet* pkt, interface* iface, const tuple& key);

/**
 * @brief Sends what the windows allow now (RFC 9293 3.8.6, RFC 5681 3.1) and
 * the FIN a close left waiting once the queue is out. Takes the lock itself.
 */
int32_t output(tcp_conn* conn);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_OUTPUT_H
