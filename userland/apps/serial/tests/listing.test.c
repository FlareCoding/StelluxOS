/* listing.c on the build machine: snapshots cut into ports, and ports matched by name and arrival */
#define _POSIX_C_SOURCE 200809L

#include "listing.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_REPORTS 8
#define PATH_SIZE   256

// Lines shaped as the kernel writes them, with made-up USB IDs: name, bus, driver, VID:PID, location, serial number,
// manufacturer, product, arrival, then the received, dropped and channel fields this tool passes on
#define FTDI_LINE   "ft232bm-0001\tusb\tftdi\t1234:5678\t1.2\tFT0001\tAcme\tAcme UART\t3\t42\t0\t-"
#define BRIDGE_LINE "cp2102-0007\tusb\tcp210x\t1234:9abc\t1.5.3\t0007\t-\tUSB to UART Bridge\t7\t789\t0\t-"
#define RETURN_LINE "cp2102-0007\tusb\tcp210x\t1234:9abc\t1.5.3\t0007\t-\tUSB to UART Bridge\t9\t0\t0\t-"

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static int parse(const char* snapshot, listing* out) {
    size_t length = strlen(snapshot);
    char* text = malloc(length + 1);
    memcpy(text, snapshot, length);

    return listing_parse(text, length, out);
}

static void check_snapshots(void) {
    listing ports;
    check(parse("", &ports) == 0 && ports.count == 0, "an empty snapshot lists no port");
    listing_free(&ports);

    check(parse(FTDI_LINE, &ports) == 0 && ports.count == 1, "a line with no newline is a port");
    const listed_port* port = &ports.ports[0];
    check(strcmp(port->line, FTDI_LINE) == 0, "a port keeps its whole line, its tabs and extra fields included");
    check(strcmp(port->fields[FIELD_NAME], "ft232bm-0001") == 0 &&
              strcmp(port->fields[FIELD_PRODUCT], "Acme UART") == 0,
          "each known field is cut out");
    check(strcmp(port->fields[FIELD_ARRIVAL], "3") == 0 && port->arrival == 3, "the arrival is read as a number");
    listing_free(&ports);

    check(parse(FTDI_LINE "\n" BRIDGE_LINE "\n", &ports) == 0 && ports.count == 2, "each line is a port");
    check(strcmp(ports.ports[1].line, BRIDGE_LINE) == 0 && ports.ports[1].arrival == 7, "the second port follows");
    check(strcmp(ports.ports[1].fields[FIELD_MANUFACTURER], "-") == 0, "a missing value stays -");
    listing_free(&ports);
    check(ports.text == NULL && ports.count == 0, "a freed listing is empty");

    check(parse("cp2102-0007\tusb\tcp210x\n" BRIDGE_LINE "\nft232bm-0001\tusb\tftdi\t1234:5678\t1.2\tFT0001\tAcme\t"
                "Acme UART\tsoon\t0\t0\t-\n",
                &ports) == 0 && ports.count == 1 && ports.ports[0].arrival == 7,
          "short lines and lines with no arrival number are left out");
    listing_free(&ports);
}

static void check_matching(void) {
    listing before;
    listing after;
    parse(FTDI_LINE "\n" BRIDGE_LINE "\n", &before);
    parse(FTDI_LINE "\n" RETURN_LINE "\n", &after);

    check(listing_find(&before, "cp2102-0007") == &before.ports[1], "a port is found by name");
    check(listing_find(&before, "cp2102-0008") == NULL, "an unlisted name finds nothing");
    check(listing_holds(&after, &before.ports[0]), "a port that stayed is held");
    check(!listing_holds(&after, &before.ports[1]), "a port back under its name with a new arrival is not held");

    listing_free(&before);
    listing_free(&after);
}

static void check_offers(void) {
    listing none = { 0 };
    listing first;
    listing returned;
    parse(BRIDGE_LINE "\n", &first);
    parse(RETURN_LINE "\n", &returned);

    tried_arrival tried = { 0 };
    check(!listing_offers(&none, "cp2102-0007", &tried) && !tried.is_known, "an unlisted port is not offered");
    check(listing_offers(&first, "cp2102-0007", &tried) && tried.is_known && tried.arrival == 7,
          "a listed port is offered once, and its arrival recorded");
    check(!listing_offers(&first, "cp2102-0007", &tried), "the arrival an open tried is not offered again");

    tried.is_retrying = true;
    check(listing_offers(&first, "cp2102-0007", &tried), "the arrival whose open failed is offered again");
    check(!listing_offers(&none, "cp2102-0007", &tried) && !tried.is_retrying, "a port that leaves ends the retry");
    check(listing_offers(&returned, "cp2102-0007", &tried) && tried.arrival == 9 && !tried.is_retrying,
          "a later arrival is offered");

    listing_free(&first);
    listing_free(&returned);
}

static void check_reads(void) {
    char path[PATH_SIZE];
    snprintf(path, sizeof(path), "/tmp/serial-listing-%d", (int)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    for (int i = 0; i < 100; i++) {
        dprintf(fd, "%s\n", FTDI_LINE);
    }

    listing ports = { 0 };
    check(listing_read(fd, &ports) == 0 && ports.count == 100, "a read takes the whole snapshot from offset 0");

    bool is_rewritten = ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0 && dprintf(fd, "%s\n", BRIDGE_LINE) > 0;
    check(is_rewritten && listing_read(fd, &ports) == 0 && ports.count == 1 && ports.ports[0].arrival == 7,
          "a second read replaces the first snapshot");

    close(fd);
    unlink(path);
    check(listing_read(fd, &ports) == -1 && ports.count == 1, "a failed read keeps the snapshot it had");
    listing_free(&ports);
}

int main(void) {
    check_snapshots();
    check_matching();
    check_offers();
    check_reads();

    if (g_failures != 0) {
        printf("listing: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("listing: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
