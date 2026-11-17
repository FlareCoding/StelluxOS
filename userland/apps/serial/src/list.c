#define _POSIX_C_SOURCE 200809L

#include "list.h"
#include "listing.h"
#include "report.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static int report_listing_failure(const char* action, int error) {
    return report_failure("-", FAILURE_IO, "cannot %s %s: %s", action, LISTING_PATH, strerror(error));
}

/* Departures first, so a port that left and came back between two snapshots reads as removed, then added */
static void report_changes(const listing* before, const listing* after) {
    for (size_t i = 0; i < before->count; i++) {
        if (!listing_holds(after, &before->ports[i])) {
            report_removed(&before->ports[i]);
        }
    }

    for (size_t i = 0; i < after->count; i++) {
        if (!listing_holds(before, &after->ports[i])) {
            report_added(&after->ports[i]);
        }
    }
}

/* Waits for a port to arrive or leave, then reads the listing into `now` */
static int wait_for_change(int fd, listing* now) {
    struct pollfd change = { .fd = fd, .events = POLLIN };
    if (poll(&change, 1, -1) < 0) {
        return -1;
    }

    return listing_read(fd, now);
}

int run_list(const serial_args* args) {
    report_use(stdout, args->has_events, false);

    int fd = open(LISTING_PATH, O_RDONLY);
    if (fd < 0) {
        return report_listing_failure("open", errno);
    }

    listing ports = { 0 };
    if (listing_read(fd, &ports) != 0) {
        int error = errno;
        close(fd);
        return report_listing_failure("read", error);
    }

    close(fd);
    report_ports(&ports);
    listing_free(&ports);

    return 0;
}

int run_watch(const serial_args* args) {
    report_use(stdout, args->has_events, false);

    int fd = open(LISTING_PATH, O_RDONLY);
    if (fd < 0) {
        return report_listing_failure("open", errno);
    }

    listing shown = { 0 };
    if (listing_read(fd, &shown) != 0) {
        int error = errno;
        close(fd);
        return report_listing_failure("read", error);
    }

    report_ports(&shown);

    // After each swap `now` holds the older snapshot, which the next read frees
    listing now = { 0 };
    while (wait_for_change(fd, &now) == 0) {
        report_changes(&shown, &now);
        listing older = shown;
        shown = now;
        now = older;
    }

    int error = errno;
    listing_free(&shown);
    listing_free(&now);
    close(fd);

    return report_listing_failure("watch", error);
}
