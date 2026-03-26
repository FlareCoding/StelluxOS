#include "net/ethernet.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "net/byteorder.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

namespace net {

void eth_recv(netif* iface, const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(eth_header)) {
        return;
    }

    const auto* hdr = reinterpret_cast<const eth_header*>(data);
    uint16_t ethertype = ntohs(hdr->ethertype);

    const uint8_t* payload = data + sizeof(eth_header);
    size_t payload_len = len - sizeof(eth_header);

    switch (ethertype) {
    case ETH_TYPE_ARP:
        arp_recv(iface, payload, payload_len);
        break;
    case ETH_TYPE_IPV4:
        ipv4_recv(iface, payload, payload_len);
        break;
    default:
        break;
    }
}

int32_t eth_send(netif* iface, const uint8_t* dst_mac,
                 uint16_t ethertype, const uint8_t* payload, size_t payload_len) {
    if (!iface || !iface->transmit || !dst_mac || !payload) {
        return ERR_INVAL;
    }

    size_t frame_len = sizeof(eth_header) + payload_len;
    if (frame_len > ETH_FRAME_MAX) {
        return ERR_INVAL;
    }

    auto* frame = static_cast<uint8_t*>(heap::kzalloc(frame_len));
    if (!frame) {
        return ERR_NOMEM;
    }

    auto* hdr = reinterpret_cast<eth_header*>(frame);
    string::memcpy(hdr->dst, dst_mac, MAC_ADDR_LEN);
    string::memcpy(hdr->src, iface->mac, MAC_ADDR_LEN);
    hdr->ethertype = htons(ethertype);

    string::memcpy(frame + sizeof(eth_header), payload, payload_len);

    int32_t rc = iface->transmit(iface, frame, frame_len);
    heap::kfree(frame);
    return rc;
}

} // namespace net
