#ifndef STELLUX_NET_INET_H
#define STELLUX_NET_INET_H

#include "common/types.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/interface.h"

namespace resource { struct resource_object; }

namespace net {
namespace inet {

// Values shared with userland through the socket system calls
constexpr uint16_t AF_INET      = 2;
constexpr uint32_t SOCK_STREAM  = 1;
constexpr uint32_t SOCK_DGRAM   = 2;
constexpr uint32_t IPPROTO_IP   = 0;
constexpr uint32_t IPPROTO_ICMP = ipv4::PROTO_ICMP;
constexpr uint32_t IPPROTO_TCP  = ipv4::PROTO_TCP;
constexpr uint32_t IPPROTO_UDP  = ipv4::PROTO_UDP;
constexpr uint32_t MSG_DONTWAIT = 0x40;

constexpr int32_t SOL_SOCKET      = 1;
constexpr int32_t SO_BROADCAST    = 6;
constexpr int32_t SO_BINDTODEVICE = 25;

constexpr size_t SOCKADDR_IN_LEN = 16;

/**
 * An IPv4 endpoint as userland passes it to the socket calls, padded to the
 * size of the generic `sockaddr`. `port` is in network byte order.
 */
struct sockaddr_in {
    uint16_t        family; // AF_INET
    uint16_t        port;
    ipv4::ipv4_addr addr;
    uint8_t         zero[8];
} __attribute__((packed));
static_assert(sizeof(sockaddr_in) == SOCKADDR_IN_LEN);

// Queries any AF_INET socket answers through ioctl, mirroring stlx/net.h
// in userland. The sizes below are the contract with it.
constexpr uint32_t SIOCGNETSTATUS = 0x4E01;
constexpr uint32_t SIOCGARPTABLE  = 0x4E02;
constexpr uint32_t SIOCSIFCONF    = 0x4E03;

constexpr size_t MAX_IFINFO  = 8;
constexpr size_t MAX_ARPINFO = 32;

constexpr uint32_t IFF_UP         = 1u << 0; // administratively enabled
constexpr uint32_t IFF_CONFIGURED = 1u << 1; // has an IPv4 address
constexpr uint32_t IFF_DEFAULT    = 1u << 2; // carries traffic no link reaches directly
constexpr uint32_t IFF_LOOPBACK   = 1u << 3;
constexpr uint32_t IFF_RUNNING    = 1u << 4; // has carrier

constexpr uint32_t ARP_RESOLVED = 1u << 0; // the hardware address is known

// One interface as userland sees it. Addresses are in host byte order.
struct ifinfo {
    char     name[IFACE_NAME_MAX];
    uint8_t  mac[eth::MAC_ADDR_LEN];
    uint8_t  pad[2];
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    uint32_t flags;
};
static_assert(sizeof(ifinfo) == 40);

struct net_status {
    uint32_t if_count;
    uint32_t reserved;
    ifinfo   interfaces[MAX_IFINFO];
};
static_assert(sizeof(net_status) == 328);

// The identity userland assigns to one interface, addresses in host byte order.
// An unspecified address clears the identity.
struct ifconf {
    char     name[IFACE_NAME_MAX];
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
};
static_assert(sizeof(ifconf) == 28);

struct arp_info_entry {
    uint32_t ipv4_addr;
    uint8_t  mac[eth::MAC_ADDR_LEN];
    uint8_t  pad[2];
    uint32_t age_ms;
    uint32_t flags;
};
static_assert(sizeof(arp_info_entry) == 20);

struct arp_info {
    uint32_t       entry_count;
    uint32_t       reserved;
    arp_info_entry entries[MAX_ARPINFO];
};
static_assert(sizeof(arp_info) == 648);

/*
 * Validates `len` bytes of a user-supplied address and extracts the endpoint.
 * `port` is returned in host byte order.
 */
int32_t parse_sockaddr(const void* addr, size_t len, ipv4::ipv4_addr* out_addr, uint16_t* out_port);

/*
 * Writes the endpoint into a buffer of `*len` bytes and sets `*len` to the
 * size written. `port` is taken in host byte order.
 */
int32_t fill_sockaddr(void* addr, size_t* len, const ipv4::ipv4_addr& ip, uint16_t port);

/*
 * Translates a network stack result into the resource layer's.
 */
int32_t map_net_error(int32_t rc);

/**
 * Creates the resource object for an AF_INET socket of `type` and `protocol`,
 * with one reference held by the caller. Speaks resource result codes, and
 * ERR_UNSUP for a combination no protocol provides.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket(uint32_t type, uint32_t protocol,
                                        resource::resource_object** out);

/**
 * @brief Answers the interface and neighbor table queries for any AF_INET socket,
 * writing the result to the user buffer at `arg`.
 * @return OK, ERR_UNSUP for a command no query matches, ERR_INVAL when the
 *         buffer cannot be written.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t socket_ioctl(resource::resource_object* obj, uint32_t cmd, uint64_t arg);

} // namespace inet
} // namespace net

#endif // STELLUX_NET_INET_H
