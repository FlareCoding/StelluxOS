#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/inet_socket.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/logging.h"
#include "common/string.h"

namespace net {

static void icmp_send_reply(netif* iface, uint32_t dst_ip,
                            const uint8_t* request_data, size_t request_len) {
    if (request_len < sizeof(icmp_header)) return;

    uint8_t reply[ETH_MTU];
    if (request_len > sizeof(reply)) return;

    string::memcpy(reply, request_data, request_len);
    auto* hdr = reinterpret_cast<icmp_header*>(reply);
    hdr->type = ICMP_TYPE_ECHO_REPLY;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->checksum = inet_checksum(reply, request_len);

    ipv4_send(iface, dst_ip, IPV4_PROTO_ICMP, reply, request_len);
}

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
        // Kernel responds to echo requests directly (standard behavior)
        icmp_send_reply(iface, src_ip, data, len);
    }

    // Deliver all ICMP packets to registered userland sockets.
    // Echo replies reach the ping app this way; echo requests are
    // also delivered so raw-socket users can see them if desired.
    deliver_to_icmp_sockets(src_ip, data, len);
}

} // namespace net
