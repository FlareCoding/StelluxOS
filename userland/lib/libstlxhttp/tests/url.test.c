/* URL splitting and redirect resolution, run on the build machine over GitHub's release URLs, ports, the longest
 * URL and the redirects a request must refuse */

#include <stlxhttp/internal/url.h>

#include <stdio.h>
#include <string.h>

#define MAX_REPORTS 8

static uint64_t     g_checks;
static uint64_t     g_failures;
static stlxhttp_url g_url;
static stlxhttp_url g_target;
static char         g_text[STLXHTTP_URL_MAX + 2];

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

static bool splits_into(const char* text, bool is_https, const char* host, uint16_t port, const char* path) {
    return stlxhttp_url_parse(text, &g_url) == STLXHTTP_OK && g_url.is_https == is_https &&
           strcmp(g_url.host, host) == 0 && g_url.port == port && strcmp(g_url.path, path) == 0;
}

static void check_parse(void) {
    check(splits_into("https://github.com/FlareCoding/StelluxOS/releases/latest/download/SHA256SUMS", true,
                      "github.com", 443, "/FlareCoding/StelluxOS/releases/latest/download/SHA256SUMS"),
          "a release URL splits");
    check(splits_into("http://10.0.2.2:18080/trunc", false, "10.0.2.2", 18080, "/trunc"), "a port is read");
    check(splits_into("https://example.com", true, "example.com", 443, "/"), "a missing path is /");
    check(splits_into("https://example.com?q=1#top", true, "example.com", 443, "/?q=1"),
          "a query keeps its slash and the fragment is dropped");

    const char* bad[] = {
        "ftp://example.com/", "example.com/", "https:///path", "https://example.com:/",
        "https://example.com:0/", "https://example.com:65536/", "https://example.com:80x/",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        check(stlxhttp_url_parse(bad[i], &g_url) == STLXHTTP_ERR_BAD_URL, bad[i]);
    }

    int length = snprintf(g_text, sizeof(g_text), "https://example.com/%0*d", STLXHTTP_URL_MAX - 20, 0);
    check(length == STLXHTTP_URL_MAX && stlxhttp_url_parse(g_text, &g_url) == STLXHTTP_OK,
          "the longest URL parses");

    snprintf(g_text, sizeof(g_text), "https://example.com/%0*d", STLXHTTP_URL_MAX - 19, 0);
    check(stlxhttp_url_parse(g_text, &g_url) == STLXHTTP_ERR_URL_TOO_LONG, "a longer URL fails");
}

static void check_resolve(void) {
    stlxhttp_url_parse("https://github.com/FlareCoding/StelluxOS/releases/latest/download/SHA256SUMS", &g_url);

    check(stlxhttp_url_resolve(&g_url, "/FlareCoding/StelluxOS/releases/download/packages-1/SHA256SUMS?x=1",
                               &g_target) == STLXHTTP_OK && strcmp(g_target.host, "github.com") == 0 &&
          strcmp(g_target.path, "/FlareCoding/StelluxOS/releases/download/packages-1/SHA256SUMS?x=1") == 0,
          "an absolute path stays on the host");
    check(stlxhttp_url_resolve(&g_url, "https://release-assets.githubusercontent.com/asset?sp=r", &g_target) ==
          STLXHTTP_OK && strcmp(g_target.host, "release-assets.githubusercontent.com") == 0,
          "an absolute URL moves to its host");
    check(stlxhttp_url_resolve(&g_url, "http://github.com/", &g_target) == STLXHTTP_ERR_DOWNGRADE,
          "https never goes to http");
    check(stlxhttp_url_resolve(&g_url, "//example.com/asset", &g_target) == STLXHTTP_ERR_BAD_URL,
          "a URL without a scheme is refused");
    check(stlxhttp_url_resolve(&g_url, "asset", &g_target) == STLXHTTP_ERR_BAD_URL,
          "a relative path is refused");

    stlxhttp_url_parse("http://example.com/", &g_url);
    check(stlxhttp_url_resolve(&g_url, "https://example.com/", &g_target) == STLXHTTP_OK && g_target.is_https,
          "http may go to https");
}

int main(void) {
    check_parse();
    check_resolve();

    if (g_failures != 0) {
        printf("url: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("url: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
