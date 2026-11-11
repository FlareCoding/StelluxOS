#include <stlxhttp/internal/url.h>

#include <string.h>

#define HTTP_PREFIX     "http://"
#define HTTPS_PREFIX    "https://"
#define PORT_DIGITS_MAX 5

static bool has_prefix(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool ends_authority(char c) {
    return c == '\0' || c == '/' || c == '?' || c == '#';
}

/* Reads the digits after the colon at `*cursor` and moves the cursor past them */
static int32_t parse_port(const char** cursor, uint16_t* port) {
    const char* digits = *cursor + 1;
    size_t count = strspn(digits, "0123456789");
    if (count == 0 || count > PORT_DIGITS_MAX || !ends_authority(digits[count])) {
        return STLXHTTP_ERR_BAD_URL;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < count; i++) {
        value = value * 10 + (uint32_t)(digits[i] - '0');
    }

    if (value == 0 || value > UINT16_MAX) {
        return STLXHTTP_ERR_BAD_URL;
    }

    *port = (uint16_t)value;
    *cursor = digits + count;

    return STLXHTTP_OK;
}

int32_t stlxhttp_url_parse(const char* text, stlxhttp_url* url) {
    if (strlen(text) > STLXHTTP_URL_MAX) {
        return STLXHTTP_ERR_URL_TOO_LONG;
    }

    const char* cursor = text;
    if (has_prefix(text, HTTPS_PREFIX)) {
        url->is_https = true;
        url->port = STLXHTTP_HTTPS_PORT;
        cursor += strlen(HTTPS_PREFIX);
    } else if (has_prefix(text, HTTP_PREFIX)) {
        url->is_https = false;
        url->port = STLXHTTP_HTTP_PORT;
        cursor += strlen(HTTP_PREFIX);
    } else {
        return STLXHTTP_ERR_BAD_URL;
    }

    size_t host_length = strcspn(cursor, ":/?#");
    if (host_length == 0 || host_length > STLXHTTP_HOST_MAX) {
        return STLXHTTP_ERR_BAD_URL;
    }

    memcpy(url->host, cursor, host_length);
    url->host[host_length] = '\0';
    cursor += host_length;

    if (*cursor == ':') {
        int32_t rc = parse_port(&cursor, &url->port);
        if (rc != STLXHTTP_OK) {
            return rc;
        }
    }

    size_t path_start = *cursor == '/' ? 0 : 1;
    size_t path_length = strcspn(cursor, "#");
    url->path[0] = '/';
    memcpy(url->path + path_start, cursor, path_length);
    url->path[path_start + path_length] = '\0';

    return STLXHTTP_OK;
}

int32_t stlxhttp_url_resolve(const stlxhttp_url* base, const char* location, stlxhttp_url* target) {
    if (location[0] == '/' && location[1] != '/') {
        size_t path_length = strcspn(location, "#");
        if (path_length > STLXHTTP_URL_MAX) {
            return STLXHTTP_ERR_URL_TOO_LONG;
        }

        *target = *base;
        memcpy(target->path, location, path_length);
        target->path[path_length] = '\0';
        return STLXHTTP_OK;
    }

    int32_t rc = stlxhttp_url_parse(location, target);
    if (rc != STLXHTTP_OK) {
        return rc;
    }

    if (base->is_https && !target->is_https) {
        return STLXHTTP_ERR_DOWNGRADE;
    }

    return STLXHTTP_OK;
}
