#ifndef SERIAL_LISTING_H
#define SERIAL_LISTING_H

/* The ports /dev/sysinfo/serial lists, read a whole snapshot at a time */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LISTING_PATH "/dev/sysinfo/serial"

// The listing's fields this tool reads, in the listing's order. A line may hold more, which --events passes on
typedef enum {
    FIELD_NAME,
    FIELD_BUS,
    FIELD_DRIVER,
    FIELD_USB_IDS,
    FIELD_LOCATION,
    FIELD_SERIAL_NUMBER,
    FIELD_MANUFACTURER,
    FIELD_PRODUCT,
    FIELD_ARRIVAL,
    KNOWN_FIELDS,
} listing_field;

typedef struct {
    const char* line;                 // the whole line without its newline
    const char* fields[KNOWN_FIELDS]; // "-" where the port has no value, as the listing writes it
    uint64_t    arrival;              // the listing's generation when the port arrived
} listed_port;

typedef struct {
    char*        text;  // the snapshot's bytes, owned
    char*        split; // a copy of text cut into fields, owned
    listed_port* ports; // owned
    size_t       count;
} listing;

// The arrival of a port that an open last tried
typedef struct {
    bool     is_known;    // a snapshot before an open listed the port
    uint64_t arrival;     // the arrival that snapshot listed
    bool     is_retrying; // the open of that arrival failed while it stayed listed, so it is offered again
} tried_arrival;

/**
 * @brief Cuts a snapshot's bytes into ports. Takes `text`, which must hold `length` bytes and room for a NUL, and frees
 * it on failure. A line with fewer than KNOWN_FIELDS fields, or with no number for its arrival, is left out.
 * @return 0, or -1 with ENOMEM.
 */
int listing_parse(char* text, size_t length, listing* out);

/**
 * @brief Takes a new snapshot of the listing file `fd` into `out`, freeing what `out` held, so `out` must be zeroed
 * before the first call. Reads from offset 0 to the end, since only a read at offset 0 takes a new snapshot, which
 * clears the file's POLLIN until the next change.
 * @return 0, or -1 with errno set, leaving `out` as it was.
 */
int listing_read(int fd, listing* out);

/**
 * @brief The port named `name`, or NULL.
 */
const listed_port* listing_find(const listing* in, const char* name);

/**
 * @brief Whether `in` lists `port` by the same name and arrival, so a port that left and came back after `port`'s
 * snapshot is not held.
 */
bool listing_holds(const listing* in, const listed_port* port);

/**
 * @brief Whether `in` offers the port named `name` to open: under an arrival `tried` does not know, or under the one
 * whose open failed. Records the arrival offered, and ends a retry once `in` no longer lists the port.
 */
bool listing_offers(const listing* in, const char* name, tried_arrival* tried);

void listing_free(listing* in);

#endif /* SERIAL_LISTING_H */
