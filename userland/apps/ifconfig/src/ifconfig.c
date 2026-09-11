#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
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

static int show(void) {
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

        if (i + 1 < st.if_count) printf("\r\n");
    }

    return 0;
}

static int parse_ip(const char* text, uint32_t* out) {
    struct in_addr addr;
    if (inet_pton(AF_INET, text, &addr) != 1) {
        printf("ifconfig: '%s' is not an IPv4 address\r\n", text);
        return -1;
    }

    *out = ntohl(addr.s_addr);
    return 0;
}

static int apply(const struct stlx_ifconf* conf) {
    if (stlx_net_set_config(conf) != 0) {
        printf("ifconfig: %s: %s\r\n", conf->name, strerror(errno));
        return 1;
    }

    return 0;
}

static int set_config(int argc, char** argv) {
    if (argc < 5 || argc > 6) {
        printf("Usage: ifconfig set <interface> <address> <netmask> [gateway]\r\n");
        return 1;
    }

    struct stlx_ifconf conf = {0};
    snprintf(conf.name, sizeof(conf.name), "%s", argv[2]);

    if (parse_ip(argv[3], &conf.ipv4_addr) != 0 || parse_ip(argv[4], &conf.ipv4_netmask) != 0) {
        return 1;
    }

    if (argc == 6 && parse_ip(argv[5], &conf.ipv4_gateway) != 0) {
        return 1;
    }

    return apply(&conf);
}

static int clear_config(int argc, char** argv) {
    if (argc != 3) {
        printf("Usage: ifconfig clear <interface>\r\n");
        return 1;
    }

    struct stlx_ifconf conf = {0};
    snprintf(conf.name, sizeof(conf.name), "%s", argv[2]);
    return apply(&conf);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc == 1) {
        return show();
    }

    if (strcmp(argv[1], "set") == 0) {
        return set_config(argc, argv);
    }

    if (strcmp(argv[1], "clear") == 0) {
        return clear_config(argc, argv);
    }

    printf("Usage: ifconfig [set <interface> <address> <netmask> [gateway] | clear <interface>]\r\n");
    return 1;
}
