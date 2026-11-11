#define _POSIX_C_SOURCE 200809L

#include <stlxhttp/internal/reply.h>

#include <string.h>
#include <strings.h>

#define STATUS_LINE_PREFIX "HTTP/"
#define STATUS_DIGITS      3
#define LENGTH_DIGITS_MAX  18  /* every length below 10^18 fits an int64_t */

void stlxhttp_reply_init(stlxhttp_reply* reply) {
    memset(reply, 0, sizeof(*reply));
    reply->state = STLXHTTP_REPLY_STATUS_LINE;
    reply->content_length = -1;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* Keeps the start of a line, which is all a status line or a header name needs */
static void append_text(stlxhttp_reply* reply, char c) {
    if (reply->text_length < STLXHTTP_REPLY_TEXT_MAX) {
        reply->text[reply->text_length++] = c;
    }
}

/* Picks what the value after a header name's colon fills, nothing for the headers the reply ignores */
static stlxhttp_reply_state state_after_name(stlxhttp_reply* reply) {
    reply->text[reply->text_length] = '\0';
    if (strcasecmp(reply->text, "Location") == 0) {
        reply->location_length = 0;
        return STLXHTTP_REPLY_LOCATION;
    }

    if (strcasecmp(reply->text, "Content-Length") == 0) {
        return STLXHTTP_REPLY_CONTENT_LENGTH;
    }

    if (strcasecmp(reply->text, "Transfer-Encoding") == 0) {
        reply->chunked = true;
    }

    return STLXHTTP_REPLY_SKIPPED_LINE;
}

static void take_name_char(stlxhttp_reply* reply, char c) {
    if (c == ':') {
        reply->state = state_after_name(reply);
        reply->text_length = 0;
    } else if (reply->text_length == STLXHTTP_REPLY_TEXT_MAX) {
        reply->state = STLXHTTP_REPLY_SKIPPED_LINE;
    } else {
        append_text(reply, c);
    }
}

static int32_t take_location_char(stlxhttp_reply* reply, char c) {
    if (reply->location_length == STLXHTTP_URL_MAX) {
        return STLXHTTP_ERR_URL_TOO_LONG;
    }

    if (reply->location_length > 0 || !is_space(c)) {
        reply->location[reply->location_length++] = c;
    }

    return STLXHTTP_OK;
}

static int32_t take_length_char(stlxhttp_reply* reply, char c) {
    if (is_space(c)) {
        return STLXHTTP_OK;
    }

    if (!is_digit(c) || reply->text_length == LENGTH_DIGITS_MAX) {
        return STLXHTTP_ERR_BAD_REPLY;
    }

    reply->text[reply->text_length++] = c;

    return STLXHTTP_OK;
}

static int32_t take_char(stlxhttp_reply* reply, char c) {
    switch (reply->state) {
    case STLXHTTP_REPLY_STATUS_LINE:
        append_text(reply, c);
        return STLXHTTP_OK;

    case STLXHTTP_REPLY_HEADER_NAME:
        take_name_char(reply, c);
        return STLXHTTP_OK;

    case STLXHTTP_REPLY_LOCATION:
        return take_location_char(reply, c);

    case STLXHTTP_REPLY_CONTENT_LENGTH:
        return take_length_char(reply, c);

    default:
        return STLXHTTP_OK;
    }
}

/* "HTTP/1.1 302 Found" gives 302 */
static int32_t parse_status_line(stlxhttp_reply* reply) {
    reply->text[reply->text_length] = '\0';
    if (strncmp(reply->text, STATUS_LINE_PREFIX, strlen(STATUS_LINE_PREFIX)) != 0) {
        return STLXHTTP_ERR_BAD_REPLY;
    }

    const char* code = strchr(reply->text, ' ');
    if (!code || strspn(code + 1, "0123456789") != STATUS_DIGITS) {
        return STLXHTTP_ERR_BAD_REPLY;
    }

    for (int i = 1; i <= STATUS_DIGITS; i++) {
        reply->status = reply->status * 10 + (code[i] - '0');
    }

    return STLXHTTP_OK;
}

static int32_t parse_content_length(stlxhttp_reply* reply) {
    if (reply->text_length == 0) {
        return STLXHTTP_ERR_BAD_REPLY;
    }

    reply->content_length = 0;
    for (size_t i = 0; i < reply->text_length; i++) {
        reply->content_length = reply->content_length * 10 + (reply->text[i] - '0');
    }

    return STLXHTTP_OK;
}

static void end_location(stlxhttp_reply* reply) {
    while (reply->location_length > 0 && is_space(reply->location[reply->location_length - 1])) {
        reply->location_length--;
    }

    reply->location[reply->location_length] = '\0';
}

static int32_t end_line(stlxhttp_reply* reply) {
    if (reply->state == STLXHTTP_REPLY_HEADER_NAME && reply->text_length == 0) {
        reply->state = STLXHTTP_REPLY_BODY;
        return STLXHTTP_OK;
    }

    int32_t rc = STLXHTTP_OK;
    if (reply->state == STLXHTTP_REPLY_STATUS_LINE) {
        rc = parse_status_line(reply);
    } else if (reply->state == STLXHTTP_REPLY_HEADER_NAME) {
        rc = STLXHTTP_ERR_BAD_REPLY;
    } else if (reply->state == STLXHTTP_REPLY_LOCATION) {
        end_location(reply);
    } else if (reply->state == STLXHTTP_REPLY_CONTENT_LENGTH) {
        rc = parse_content_length(reply);
    }

    reply->state = STLXHTTP_REPLY_HEADER_NAME;
    reply->text_length = 0;

    return rc;
}

int64_t stlxhttp_reply_parse_header(stlxhttp_reply* reply, const uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char c = (char)bytes[i];
        if (c == '\r') {
            continue;
        }

        int32_t rc = c == '\n' ? end_line(reply) : take_char(reply, c);
        if (rc != STLXHTTP_OK) {
            return rc;
        }

        if (reply->state == STLXHTTP_REPLY_BODY) {
            return (int64_t)(i + 1);
        }
    }

    return (int64_t)length;
}

size_t stlxhttp_reply_take_body(stlxhttp_reply* reply, size_t length) {
    if (reply->content_length >= 0 && (int64_t)length > reply->content_length - reply->body_received) {
        length = (size_t)(reply->content_length - reply->body_received);
    }

    reply->body_received += (int64_t)length;

    return length;
}

bool stlxhttp_reply_body_complete(const stlxhttp_reply* reply) {
    return reply->content_length >= 0 && reply->body_received == reply->content_length;
}
