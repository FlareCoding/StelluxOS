/* The reply parser, run on the build machine over the replies GitHub sends and the edges around them: a header
 * split across reads at every byte, a header line longer than any buffer, and bodies cut short or run long */

#include <stlxhttp/internal/reply.h>

#include <stdio.h>
#include <string.h>

#define MAX_REPORTS        8
#define POLICY_LINE_LENGTH 4173  /* github.com's Content-Security-Policy line */
#define LOCATION_REPLY_MAX (STLXHTTP_URL_MAX + 64)
#define LONG_LINE_PIECE    1000

static const char REDIRECT_REPLY[] =
    "HTTP/1.1 302 Found\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Location: https://release-assets.githubusercontent.com/asset?sp=r&se=2026-11-11T02%3A08%3A00Z\r\n"
    "Content-Length: 0\r\n"
    "connection: close\r\n"
    "\r\n";

static const char REDIRECT_TARGET[] =
    "https://release-assets.githubusercontent.com/asset?sp=r&se=2026-11-11T02%3A08%3A00Z";

static const char ASSET_REPLY[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Length: 3605222\r\n"
    "Content-Type: application/octet-stream\r\n"
    "\r\n"
    "body";

static uint64_t g_checks;
static uint64_t g_failures;
static char     g_text[LOCATION_REPLY_MAX];

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

/* Hands `text` over in pieces of `piece` bytes and returns the header's length, or the error, or
 * STLXHTTP_ERR_TRUNCATED when the header never ends */
static int64_t parse_in_pieces(stlxhttp_reply* reply, const char* text, size_t length, size_t piece) {
    stlxhttp_reply_init(reply);
    size_t offset = 0;
    while (offset < length) {
        size_t size = length - offset < piece ? length - offset : piece;
        int64_t taken = stlxhttp_reply_parse_header(reply, (const uint8_t*)text + offset, size);
        if (taken < 0) {
            return taken;
        }

        offset += (size_t)taken;
        if (reply->state == STLXHTTP_REPLY_BODY) {
            return (int64_t)offset;
        }
    }

    return STLXHTTP_ERR_TRUNCATED;
}

static int64_t parse_whole(stlxhttp_reply* reply, const char* text) {
    return parse_in_pieces(reply, text, strlen(text), strlen(text));
}

static void check_split_anywhere(void) {
    static stlxhttp_reply reply;
    size_t length = strlen(REDIRECT_REPLY);
    for (size_t piece = 1; piece <= length; piece++) {
        int64_t header = parse_in_pieces(&reply, REDIRECT_REPLY, length, piece);
        check(header == (int64_t)length && reply.status == 302 && reply.content_length == 0 &&
              strcmp(reply.location, REDIRECT_TARGET) == 0, "a redirect split anywhere parses the same");
    }
}

static void check_asset_reply(void) {
    static stlxhttp_reply reply;
    int64_t header = parse_whole(&reply, ASSET_REPLY);
    check(header == (int64_t)strlen(ASSET_REPLY) - 4, "the header ends before the body");
    check(reply.status == 200 && reply.content_length == 3605222, "a 200 keeps its status and length");
    check(!reply.chunked && reply.location[0] == '\0', "a 200 without those headers is neither chunked nor moved");
}

static void check_long_line(void) {
    static stlxhttp_reply reply;
    int length = snprintf(g_text, sizeof(g_text), "HTTP/1.1 302 Found\r\nContent-Security-Policy: %0*d\r\n"
                          "Location: \t/next \t\r\nContent-Length: 0\r\n\r\n", POLICY_LINE_LENGTH, 0);
    int64_t header = parse_in_pieces(&reply, g_text, (size_t)length, LONG_LINE_PIECE);
    check(header == length, "a header line of any length is skipped");
    check(strcmp(reply.location, "/next") == 0, "the spaces around a Location are dropped");
}

static void check_location_limit(void) {
    static stlxhttp_reply reply;
    int length = snprintf(g_text, sizeof(g_text), "HTTP/1.1 302 Found\r\nLocation: /%0*d\r\n\r\n",
                          STLXHTTP_URL_MAX - 1, 0);
    check(parse_whole(&reply, g_text) == length, "a Location of the longest URL is kept");
    check(strlen(reply.location) == STLXHTTP_URL_MAX, "the longest Location is kept whole");

    snprintf(g_text, sizeof(g_text), "HTTP/1.1 302 Found\r\nLocation: /%0*d\r\n\r\n", STLXHTTP_URL_MAX, 0);
    check(parse_whole(&reply, g_text) == STLXHTTP_ERR_URL_TOO_LONG, "a longer Location fails");
}

static void check_malformed(void) {
    static stlxhttp_reply reply;
    check(parse_whole(&reply, "SSH-2.0-OpenSSH\r\n\r\n") == STLXHTTP_ERR_BAD_REPLY, "a reply that is not HTTP");
    check(parse_whole(&reply, "HTTP/1.1 20 OK\r\n\r\n") == STLXHTTP_ERR_BAD_REPLY, "a two digit status");
    check(parse_whole(&reply, "HTTP/1.1 2000 OK\r\n\r\n") == STLXHTTP_ERR_BAD_REPLY, "a four digit status");

    check(parse_whole(&reply, "HTTP/1.1 200 OK\r\nno colon\r\n\r\n") == STLXHTTP_ERR_BAD_REPLY,
          "a header line without a colon");
    check(parse_whole(&reply, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n") == STLXHTTP_ERR_TRUNCATED,
          "a header without its blank line never ends");

    check(parse_whole(&reply, "HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n") == STLXHTTP_ERR_BAD_REPLY,
          "a length that is not a number");
    check(parse_whole(&reply, "HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\n") == STLXHTTP_ERR_BAD_REPLY,
          "an empty length");
    check(parse_whole(&reply, "HTTP/1.1 200 OK\r\nContent-Length: 1234567890123456789\r\n\r\n") ==
          STLXHTTP_ERR_BAD_REPLY, "a length of more than 18 digits");
}

static void check_framing(void) {
    static stlxhttp_reply reply;
    check(parse_whole(&reply, "HTTP/1.1 404 Not Found\r\nTransfer-Encoding: chunked\r\n\r\n") > 0 &&
          reply.status == 404 && reply.chunked, "a chunked reply is marked");

    const char* bare = "HTTP/1.0 200 OK\ncontent-length: 5\n\nhello";
    check(parse_whole(&reply, bare) == (int64_t)strlen(bare) - 5 && reply.content_length == 5,
          "lines ending in a bare line feed parse, and names match in any case");
}

static void check_bodies(void) {
    static stlxhttp_reply reply;
    parse_whole(&reply, "HTTP/1.0 200 OK\r\nContent-Length: 100000\r\n\r\n");
    check(stlxhttp_reply_take_body(&reply, 40000) == 40000 && !stlxhttp_reply_body_complete(&reply),
          "40000 of 100000 bytes is a short body");
    check(stlxhttp_reply_take_body(&reply, 60000) == 60000 && stlxhttp_reply_body_complete(&reply),
          "the rest completes the body");
    check(stlxhttp_reply_take_body(&reply, 10) == 0, "bytes past the stated length are dropped");

    parse_whole(&reply, "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n");
    check(stlxhttp_reply_take_body(&reply, 25) == 10 && stlxhttp_reply_body_complete(&reply),
          "a read running past the length is cut at it");

    parse_whole(&reply, "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
    check(stlxhttp_reply_body_complete(&reply), "a stated length of 0 is complete at once");

    parse_whole(&reply, "HTTP/1.0 200 OK\r\n\r\n");
    check(stlxhttp_reply_take_body(&reply, 5000) == 5000 && !stlxhttp_reply_body_complete(&reply),
          "a body without a length is never complete by count");
}

int main(void) {
    check_split_anywhere();
    check_asset_reply();
    check_long_line();
    check_location_limit();
    check_malformed();
    check_framing();

    check_bodies();

    if (g_failures != 0) {
        printf("reply: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("reply: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
