#include "net/inet.h"
#include "net/byteorder.h"
#include "net/icmp_socket.h"
#include "net/udp_socket.h"
#include "net/arp.h"
#include "net/route.h"
#include "resource/resource.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "common/string.h"

namespace net {
namespace inet {

static uint32_t to_host_order(const ipv4::ipv4_addr& addr) {
    return (static_cast<uint32_t>(addr.bytes[0]) << 24) | (static_cast<uint32_t>(addr.bytes[1]) << 16) |
           (static_cast<uint32_t>(addr.bytes[2]) << 8) | addr.bytes[3];
}

static ipv4::ipv4_addr from_host_order(uint32_t value) {
    return {{static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
             static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)}};
}

static void fill_ifinfo(interface* iface, bool is_default, ifinfo* info) {
    ipv4::ipv4_config conf = iface->ipv4_conf();

    size_t name_len = string::strlen(iface->name());
    if (name_len >= sizeof(info->name)) {
        name_len = sizeof(info->name) - 1;
    }

    string::memcpy(info->name, iface->name(), name_len);
    string::memcpy(info->mac, iface->mac().bytes, eth::MAC_ADDR_LEN);

    info->ipv4_addr = to_host_order(conf.address);
    info->ipv4_netmask = to_host_order(conf.netmask);
    info->ipv4_gateway = to_host_order(conf.gateway);

    info->flags = (iface->enabled() ? IFF_UP : 0) | (conf.configured() ? IFF_CONFIGURED : 0) |
                  (is_default ? IFF_DEFAULT : 0) | (iface->is_loopback() ? IFF_LOOPBACK : 0) |
                  (iface->link_up() ? IFF_RUNNING : 0);
}

__PRIVILEGED_CODE static int32_t query_net_status(uint64_t arg) {
    net_status status = {};
    interface* default_iface = route::default_interface();

    size_t count = interface_count();
    if (count > MAX_IFINFO) {
        count = MAX_IFINFO;
    }

    for (size_t i = 0; i < count; i++) {
        interface* iface = interface_at(i);
        fill_ifinfo(iface, iface == default_iface, &status.interfaces[i]);
    }

    status.if_count = static_cast<uint32_t>(count);

    int32_t rc = mm::uaccess::copy_to_user(reinterpret_cast<void*>(arg), &status, sizeof(status));
    return rc == mm::uaccess::OK ? resource::OK : resource::ERR_INVAL;
}

__PRIVILEGED_CODE static int32_t query_arp_table(uint64_t arg) {
    arp::arp_snapshot_entry entries[arp::TABLE_SIZE];
    size_t count = arp::snapshot(entries, arp::TABLE_SIZE);
    if (count > MAX_ARPINFO) {
        count = MAX_ARPINFO;
    }

    arp_info info = {};
    for (size_t i = 0; i < count; i++) {
        arp_info_entry& out = info.entries[i];
        out.ipv4_addr = to_host_order(entries[i].ip);

        string::memcpy(out.mac, entries[i].mac.bytes, eth::MAC_ADDR_LEN);
        
        out.age_ms = static_cast<uint32_t>(entries[i].age_ns / (arp::NS_PER_SEC / 1000));
        out.flags = entries[i].state == arp::arp_entry_state::resolved ? ARP_RESOLVED : 0;
    }

    info.entry_count = static_cast<uint32_t>(count);

    int32_t rc = mm::uaccess::copy_to_user(reinterpret_cast<void*>(arg), &info, sizeof(info));
    return rc == mm::uaccess::OK ? resource::OK : resource::ERR_INVAL;
}

__PRIVILEGED_CODE static int32_t set_interface_config(uint64_t user_request) {
    ifconf request;
    if (mm::uaccess::copy_from_user(&request, reinterpret_cast<const void*>(user_request), sizeof(request)) != mm::uaccess::OK) {
        return resource::ERR_INVAL;
    }

    request.name[IFACE_NAME_MAX - 1] = '\0';
    interface* iface = find_interface_by_name(request.name);
    if (!iface) {
        return resource::ERR_NOENT;
    }

    if (request.ipv4_addr == 0) {
        iface->unconfigure_ipv4();
        return resource::OK;
    }

    ipv4::ipv4_config conf = {
        from_host_order(request.ipv4_addr),
        from_host_order(request.ipv4_netmask),
        from_host_order(request.ipv4_gateway),
    };

    return iface->configure_ipv4(conf) == OK ? resource::OK : resource::ERR_INVAL;
}

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

int32_t map_net_error(int32_t rc) {
    switch (rc) {
    case OK:            return resource::OK;
    case ERR_INVALID:   return resource::ERR_INVAL;
    case ERR_NO_MEMORY: return resource::ERR_NOMEM;
    case ERR_TOO_LARGE: return resource::ERR_MSGSIZE;
    case ERR_NO_ROUTE:  return resource::ERR_HOSTUNREACH;
    case ERR_DOWN:      return resource::ERR_HOSTUNREACH;
    case ERR_IN_USE:    return resource::ERR_ADDRINUSE;
    case ERR_ACCESS:    return resource::ERR_ACCESS;
    default:            return resource::ERR_IO;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket(uint32_t type, uint32_t protocol,
                                        resource::resource_object** out) {
    if (!out) {
        return resource::ERR_INVAL;
    }

    // A datagram socket is UDP unless ICMP is named, which selects a ping socket
    bool is_udp = protocol == IPPROTO_UDP || protocol == IPPROTO_IP;
    if (type != SOCK_DGRAM || (!is_udp && protocol != IPPROTO_ICMP)) {
        return resource::ERR_UNSUP;
    }

    // The object stays in privileged memory with the rest of the resource
    // layer, only the protocol state behind `impl` is reachable while lowered
    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        return resource::ERR_NOMEM;
    }

    obj->type = resource::resource_type::SOCKET;
    if (is_udp) {
        obj->ops = udp::socket_ops();
        obj->impl = udp::socket_open();
    } else {
        obj->ops = icmp::socket_ops();
        obj->impl = icmp::socket_open();
    }

    if (!obj->impl) {
        heap::kfree_delete(obj);
        return resource::ERR_NOMEM;
    }

    *out = obj;
    return resource::OK;
}

__PRIVILEGED_CODE int32_t socket_ioctl(resource::resource_object*, uint32_t cmd, uint64_t arg) {
    switch (cmd) {
    case SIOCGNETSTATUS: return query_net_status(arg);
    case SIOCGARPTABLE:  return query_arp_table(arg);
    case SIOCSIFCONF:    return set_interface_config(arg);
    default:             return resource::ERR_UNSUP;
    }
}

} // namespace inet
} // namespace net
