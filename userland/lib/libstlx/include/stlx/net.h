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

#define STLX_SIOCGNETSTATUS   0x4E01

struct stlx_ifinfo {
    char     name[16];
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    uint32_t ipv4_dns;
    uint32_t flags;
};

_Static_assert(sizeof(struct stlx_ifinfo) == 44, "stlx_ifinfo ABI size mismatch");

struct stlx_net_status {
    uint32_t           if_count;
    uint32_t           _reserved;
    struct stlx_ifinfo interfaces[STLX_NET_MAX_IF];
};

_Static_assert(sizeof(struct stlx_net_status) == 360, "stlx_net_status ABI size mismatch");

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

#endif /* STLX_NET_H */
