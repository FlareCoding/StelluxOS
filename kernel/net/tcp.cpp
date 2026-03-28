#include "net/tcp.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/logging.h"

namespace net {

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

    log::info("tcp: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u [%s] seq=%u ack=%u len=%u",
              (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
              (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port,
              (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
              (dst_ip >> 8) & 0xFF, dst_ip & 0xFF, dst_port,
              flag_str, seq, ack_num, static_cast<uint32_t>(payload_len));

    // TODO (Layer 2): look up socket by dst_port, dispatch to state machine
}

} // namespace net
