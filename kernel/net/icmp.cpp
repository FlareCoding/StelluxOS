#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/inet_socket.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/net.h"
#include "common/string.h"

namespace net {

void icmp_recv(netif* iface, uint32_t src_ip, const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(icmp_header)) {
        return;
    }

    const auto* hdr = reinterpret_cast<const icmp_header*>(data);

    // Verify ICMP checksum
    uint16_t computed = inet_checksum(data, len);
    if (computed != 0) {
        return;
    }

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST && hdr->code == 0) {
        // Build the echo reply packet and queue it for deferred
        // transmission. Sending inline from RX context would recurse
        // through ipv4_send → arp_resolve → poll → deliver_rx_batch.
        if (len <= ETH_MTU) {
            uint8_t reply[ETH_MTU];
            string::memcpy(reply, data, len);
            auto* reply_hdr = reinterpret_cast<icmp_header*>(reply);
            reply_hdr->type = ICMP_TYPE_ECHO_REPLY;
            reply_hdr->code = 0;
            reply_hdr->checksum = 0;
            reply_hdr->checksum = inet_checksum(reply, len);

            queue_deferred_tx(iface, src_ip, IPV4_PROTO_ICMP, reply, len);
        }
    }

    // Deliver all ICMP packets to registered userland sockets
    deliver_to_icmp_sockets(src_ip, data, len);
}

} // namespace net
