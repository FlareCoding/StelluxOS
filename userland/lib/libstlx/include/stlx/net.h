#ifndef STLX_NET_H
#define STLX_NET_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define STLX_NET_MAX_IF       8

#define STLX_IFF_UP           (1u << 0)
#define STLX_IFF_CONFIGURED   (1u << 1)
#define STLX_IFF_DEFAULT      (1u << 2)
#define STLX_IFF_LOOPBACK     (1u << 3)
#define STLX_IFF_RUNNING      (1u << 4)

#define STLX_SIOCGNETSTATUS   0x4E01

struct stlx_ifinfo {
    char     name[16];
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    uint32_t flags;
};

_Static_assert(sizeof(struct stlx_ifinfo) == 40, "stlx_ifinfo ABI size mismatch");

struct stlx_net_status {
    uint32_t           if_count;
    uint32_t           _reserved;
    struct stlx_ifinfo interfaces[STLX_NET_MAX_IF];
};

_Static_assert(sizeof(struct stlx_net_status) == 328, "stlx_net_status ABI size mismatch");

static inline int stlx_net_get_status(struct stlx_net_status* out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int rc = ioctl(fd, STLX_SIOCGNETSTATUS, out);
    close(fd);
    return rc;
}

static inline const struct stlx_ifinfo*
stlx_net_default_if(const struct stlx_net_status* st) {
    for (uint32_t i = 0; i < st->if_count; i++) {
        if (st->interfaces[i].flags & STLX_IFF_DEFAULT)
            return &st->interfaces[i];
    }

    return NULL;
}

/* --- ARP table query --- */

#define STLX_ARP_TABLE_SIZE   32
#define STLX_SIOCGARPTABLE    0x4E02

#define STLX_ARP_RESOLVED     (1u << 0)

struct stlx_arp_entry {
    uint32_t ipv4_addr;
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t age_ms;
    uint32_t flags;
};

_Static_assert(sizeof(struct stlx_arp_entry) == 20, "stlx_arp_entry ABI size mismatch");

struct stlx_arp_table {
    uint32_t              entry_count;
    uint32_t              _reserved;
    struct stlx_arp_entry entries[STLX_ARP_TABLE_SIZE];
};

_Static_assert(sizeof(struct stlx_arp_table) == 648, "stlx_arp_table ABI size mismatch");

static inline int stlx_arp_get_table(struct stlx_arp_table* out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int rc = ioctl(fd, STLX_SIOCGARPTABLE, out);
    close(fd);
    return rc;
}

/* --- Interface configuration --- */

#define STLX_SIOCSIFCONF      0x4E03

/* Addresses in host byte order, an ipv4_addr of zero clears the identity */
struct stlx_ifconf {
    char     name[16];
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
};

_Static_assert(sizeof(struct stlx_ifconf) == 28, "stlx_ifconf ABI size mismatch");

static inline int stlx_net_set_config(const struct stlx_ifconf* conf) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int rc = ioctl(fd, STLX_SIOCSIFCONF, conf);
    close(fd);
    return rc;
}

/* --- TCP tables and counters --- */

#define STLX_SIOCGTCPINFO     0x4E04

#define STLX_TCP_KIND_LISTENER    0
#define STLX_TCP_KIND_REQUEST     1
#define STLX_TCP_KIND_CONNECTION  2
#define STLX_TCP_KIND_TIMEWAIT    3

#define STLX_TCP_CLOSED       0
#define STLX_TCP_LISTEN       1
#define STLX_TCP_SYN_SENT     2
#define STLX_TCP_SYN_RCVD     3
#define STLX_TCP_ESTABLISHED  4
#define STLX_TCP_FIN_WAIT_1   5
#define STLX_TCP_FIN_WAIT_2   6
#define STLX_TCP_CLOSE_WAIT   7
#define STLX_TCP_CLOSING      8
#define STLX_TCP_LAST_ACK     9
#define STLX_TCP_TIME_WAIT    10

#define STLX_TCP_TIMER_NONE      0
#define STLX_TCP_TIMER_RTO       1
#define STLX_TCP_TIMER_ORPHAN    2
#define STLX_TCP_TIMER_TIMEWAIT  3

#define STLX_TCP_TIMESTAMPS    (1u << 0)
#define STLX_TCP_SACK          (1u << 1)
#define STLX_TCP_WINDOW_SCALE  (1u << 2)
#define STLX_TCP_ORPHANED      (1u << 3)
#define STLX_TCP_FIN_SENT      (1u << 4)
#define STLX_TCP_FIN_RCVD      (1u << 5)

/* One listener, request, connection, or TIME_WAIT entry. Addresses and ports
 * in host byte order, fields a kind does not have are zero. */
struct stlx_tcp_record {
    uint8_t  kind;
    uint8_t  state;
    uint8_t  timer_kind;
    uint8_t  flags;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_addr;
    uint32_t remote_addr;
    char     iface[16];
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t snd_wnd;
    uint32_t rcv_wnd;
    uint16_t snd_mss;
    uint8_t  snd_wscale;
    uint8_t  rcv_wscale;
    uint16_t backlog;
    uint16_t requests;
    uint16_t accepted;
    uint8_t  retransmits;
    uint8_t  _pad;
    uint32_t timer_ms;
    int32_t  error;
};

_Static_assert(sizeof(struct stlx_tcp_record) == 72, "stlx_tcp_record ABI size mismatch");

struct stlx_tcp_counters {
    uint64_t segments_in;
    uint64_t segments_out;
    uint64_t retransmits;
    uint64_t rsts_sent;
    uint64_t rsts_received;
    uint64_t checksum_failures;
    uint64_t listen_drops;
    uint64_t paws_drops;
    uint64_t challenge_acks;
    uint32_t listeners;
    uint32_t requests;
    uint32_t connections;
    uint32_t timewaits;
};

_Static_assert(sizeof(struct stlx_tcp_counters) == 88, "stlx_tcp_counters ABI size mismatch");

/* The caller sets capacity and records, the kernel fills count records there,
 * reports in total how many existed, and fills the counters. */
struct stlx_tcp_info {
    uint32_t                 capacity;
    uint32_t                 count;
    uint32_t                 total;
    uint32_t                 _reserved;
    uint64_t                 records;
    struct stlx_tcp_counters counters;
};

_Static_assert(sizeof(struct stlx_tcp_info) == 112, "stlx_tcp_info ABI size mismatch");

static inline int stlx_tcp_get_info(struct stlx_tcp_record* records, uint32_t capacity,
                                    struct stlx_tcp_info* out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    out->capacity = capacity;
    out->records = (uint64_t)(uintptr_t)records;
    int rc = ioctl(fd, STLX_SIOCGTCPINFO, out);
    close(fd);
    return rc;
}

#endif /* STLX_NET_H */
