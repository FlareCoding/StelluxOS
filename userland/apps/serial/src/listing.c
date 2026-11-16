#define _POSIX_C_SOURCE 200809L

#include "listing.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIRST_READ_BYTES 4096

/* Cuts `line` at every tab and keeps the first fields, as many as fit, returning how many there were */
static size_t split_fields(char* line, const char* fields[], size_t capacity) {
    size_t count = 0;
    char* field = line;
    while (true) {
        if (count < capacity) {
            fields[count] = field;
        }

        count++;
        char* tab = strchr(field, '\t');
        if (!tab) {
            return count;
        }

        *tab = '\0';
        field = tab + 1;
    }
}

static bool parse_arrival(const char* text, uint64_t* arrival) {
    if (*text < '0' || *text > '9') {
        return false;
    }

    char* end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') {
        return false;
    }

    *arrival = value;
    return true;
}

static size_t count_lines(const char* text, size_t length) {
    size_t count = 1;
    for (size_t i = 0; i < length; i++) {
        count += text[i] == '\n';
    }

    return count;
}

int listing_parse(char* text, size_t length, listing* out) {
    text[length] = '\0';
    char* split = malloc(length + 1);
    listed_port* ports = calloc(count_lines(text, length), sizeof(*ports));
    if (!split || !ports) {
        free(split);
        free(ports);
        free(text);
        errno = ENOMEM;
        return -1;
    }

    memcpy(split, text, length + 1);
    size_t count = 0;
    size_t start = 0;
    while (start < length) {
        char* newline = memchr(text + start, '\n', length - start);
        size_t end = newline ? (size_t)(newline - text) : length;
        text[end] = '\0';
        split[end] = '\0';

        listed_port* port = &ports[count];
        port->line = text + start;
        bool is_whole = split_fields(split + start, port->fields, KNOWN_FIELDS) >= KNOWN_FIELDS;
        if (is_whole && parse_arrival(port->fields[FIELD_ARRIVAL], &port->arrival)) {
            count++;
        }

        start = end + 1;
    }

    *out = (listing){ .text = text, .split = split, .ports = ports, .count = count };
    return 0;
}

int listing_read(int fd, listing* out) {
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return -1;
    }

    size_t capacity = FIRST_READ_BYTES;
    size_t length = 0;
    char* text = malloc(capacity + 1);
    if (!text) {
        errno = ENOMEM;
        return -1;
    }

    while (true) {
        if (length == capacity) {
            char* larger = realloc(text, 2 * capacity + 1);
            if (!larger) {
                free(text);
                errno = ENOMEM;
                return -1;
            }

            text = larger;
            capacity *= 2;
        }

        ssize_t taken = read(fd, text + length, capacity - length);
        if (taken < 0) {
            int error = errno;
            free(text);
            errno = error;
            return -1;
        }

        if (taken == 0) {
            break;
        }

        length += (size_t)taken;
    }

    listing fresh;
    if (listing_parse(text, length, &fresh) != 0) {
        return -1;
    }

    listing_free(out);
    *out = fresh;
    return 0;
}

const listed_port* listing_find(const listing* in, const char* name) {
    for (size_t i = 0; i < in->count; i++) {
        if (strcmp(in->ports[i].fields[FIELD_NAME], name) == 0) {
            return &in->ports[i];
        }
    }

    return NULL;
}

bool listing_holds(const listing* in, const listed_port* port) {
    const listed_port* listed = listing_find(in, port->fields[FIELD_NAME]);
    return listed && listed->arrival == port->arrival;
}

void listing_free(listing* in) {
    free(in->text);
    free(in->split);
    free(in->ports);
    *in = (listing){ 0 };
}
