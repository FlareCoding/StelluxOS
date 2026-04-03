#include <stdio.h>
#include <stlx/net.h>

static void format_ip(uint32_t ip, char* buf, size_t sz) {
    snprintf(buf, sz, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

static void print_flags(uint32_t flags) {
    int first = 1;
    if (flags & STLX_IFF_UP)         { printf("%sUP", first ? "" : ","); first = 0; }
    if (flags & STLX_IFF_CONFIGURED) { printf("%sCONFIGURED", first ? "" : ","); first = 0; }
    if (flags & STLX_IFF_DEFAULT)    { printf("%sDEFAULT", first ? "" : ","); first = 0; }
    if (flags & STLX_IFF_LOOPBACK)   { printf("%sLOOPBACK", first ? "" : ","); first = 0; }
    if (first) printf("NONE");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    struct stlx_net_status st;
    if (stlx_net_get_status(&st) != 0) {
        printf("ifconfig: failed to query network status\r\n");
        return 1;
    }

    if (st.if_count == 0) {
        printf("No network interfaces found.\r\n");
        return 0;
    }

    for (uint32_t i = 0; i < st.if_count; i++) {
        const struct stlx_ifinfo* iface = &st.interfaces[i];

        printf("%s: flags=", iface->name);
        print_flags(iface->flags);
        printf("\r\n");

        if (iface->flags & STLX_IFF_CONFIGURED) {
            char ip[16], mask[16], gw[16];
            format_ip(iface->ipv4_addr, ip, sizeof(ip));
            format_ip(iface->ipv4_netmask, mask, sizeof(mask));
            format_ip(iface->ipv4_gateway, gw, sizeof(gw));
            printf("      inet %s  netmask %s  gateway %s\r\n", ip, mask, gw);
        }

        printf("      ether %02x:%02x:%02x:%02x:%02x:%02x\r\n",
               iface->mac[0], iface->mac[1], iface->mac[2],
               iface->mac[3], iface->mac[4], iface->mac[5]);

        if (iface->ipv4_dns != 0) {
            char dns[16];
            format_ip(iface->ipv4_dns, dns, sizeof(dns));
            printf("      dns %s\r\n", dns);
        }

        if (i + 1 < st.if_count) printf("\r\n");
    }

    return 0;
}
