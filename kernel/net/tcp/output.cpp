#include "net/tcp/output.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/route.h"
#include "net/byteorder.h"
#include "common/string.h"

namespace net {
namespace tcp {

int32_t send_segment(interface* iface, const tuple& key, uint8_t flags, uint32_t seq, uint32_t ack,
                     uint16_t window, const tcp_options& opts) {
    route::route_result route;
    int32_t rc = route::lookup_on(iface, key.remote_addr, &route);
    if (rc != OK) {
        return rc;
    }

    route.source = key.local_addr;

    packet* pkt = packet::alloc();
    if (!pkt) {
        return ERR_NO_MEMORY;
    }

    uint8_t options[MAX_OPTIONS_LEN];
    size_t options_len = build_options(options, opts);
    size_t header_len = HEADER_LEN + options_len;

    tcp_header* hdr = nullptr;
    if (pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN)) {
        hdr = reinterpret_cast<tcp_header*>(pkt->put(header_len));
    }

    if (!hdr) {
        packet::free(pkt);
        return ERR_TOO_LARGE;
    }

    *hdr = {};
    hdr->src_port = htons(key.local_port);
    hdr->dst_port = htons(key.remote_port);
    hdr->seq = htonl(seq);
    hdr->ack = htonl(ack);
    hdr->set_data_offset(static_cast<uint8_t>(header_len / sizeof(uint32_t)));
    hdr->flags = flags;
    hdr->window = htons(window);
    string::memcpy(reinterpret_cast<uint8_t*>(hdr) + HEADER_LEN, options, options_len);
    hdr->checksum = htons(compute_checksum(key.local_addr, key.remote_addr, hdr, header_len));

    return ipv4::output(pkt, key.remote_addr, route, ipv4::PROTO_TCP);
}

} // namespace tcp
} // namespace net
