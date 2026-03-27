#include <stdio.h>
#include <stlx/net.h>

static void format_ip(uint32_t ip, char* buf, size_t sz) {
    snprintf(buf, sz, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    struct stlx_arp_table table;
    if (stlx_arp_get_table(&table) != 0) {
        printf("arp: failed to query ARP table\r\n");
        return 1;
    }

    if (table.entry_count == 0) {
        printf("ARP table is empty.\r\n");
        return 0;
    }

    printf("%-17s  %-19s  %s\r\n", "Address", "HWaddress", "Age");

    for (uint32_t i = 0; i < table.entry_count; i++) {
        const struct stlx_arp_entry* e = &table.entries[i];
        char ip[16];
        format_ip(e->ipv4_addr, ip, sizeof(ip));

        printf("%-17s  %02x:%02x:%02x:%02x:%02x:%02x  %ums\r\n",
               ip,
               e->mac[0], e->mac[1], e->mac[2],
               e->mac[3], e->mac[4], e->mac[5],
               e->age_ms);
    }

    return 0;
}
