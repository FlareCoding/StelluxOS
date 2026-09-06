#include "net/inet.h"
#include "net/byteorder.h"
#include "net/icmp_socket.h"
#include "resource/resource.h"
#include "mm/heap.h"
#include "common/string.h"

namespace net {
namespace inet {

int32_t parse_sockaddr(const void* addr, size_t len, ipv4::ipv4_addr* out_addr, uint16_t* out_port) {
    if (!addr || !out_addr || !out_port || len < SOCKADDR_IN_LEN) {
        return ERR_INVALID;
    }

    const sockaddr_in* sa = static_cast<const sockaddr_in*>(addr);
    if (sa->family != AF_INET) {
        return ERR_INVALID;
    }

    *out_addr = sa->addr;
    *out_port = ntohs(sa->port);
    return OK;
}

int32_t fill_sockaddr(void* addr, size_t* len, const ipv4::ipv4_addr& ip, uint16_t port) {
    if (!addr || !len || *len < SOCKADDR_IN_LEN) {
        return ERR_INVALID;
    }

    sockaddr_in* sa = static_cast<sockaddr_in*>(addr);
    sa->family = AF_INET;
    sa->port = htons(port);
    sa->addr = ip;
    string::memset(sa->zero, 0, sizeof(sa->zero));

    *len = SOCKADDR_IN_LEN;
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket(uint32_t type, uint32_t protocol,
                                        resource::resource_object** out) {
    if (!out) {
        return resource::ERR_INVAL;
    }

    if (type != SOCK_DGRAM || protocol != IPPROTO_ICMP) {
        return resource::ERR_UNSUP;
    }

    icmp::icmp_socket* sock = icmp::socket_open();
    if (!sock) {
        return resource::ERR_NOMEM;
    }

    // The object stays in privileged memory with the rest of the resource
    // layer, only the protocol state behind `impl` is reachable while lowered
    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        icmp::socket_close(sock);
        return resource::ERR_NOMEM;
    }

    obj->type = resource::resource_type::SOCKET;
    obj->ops = icmp::socket_ops();
    obj->impl = sock;

    *out = obj;
    return resource::OK;
}

} // namespace inet
} // namespace net
