/* What a drop pastes: files win over text, each file's path quoted so the Stellux shell and POSIX shells read it
 * whole, file URIs of another computer and names holding control characters left out */
#include "drop.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REPORTS 8

static uint64_t g_checks;
static uint64_t g_failures;

static void check(int holds, const char *what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static stlxwin_offer_types types_of(const char *first, const char *second) {
    stlxwin_offer_types types;
    memset(&types, 0, sizeof(types));
    const char *names[] = { first, second };
    for (int i = 0; i < 2 && names[i]; i++) {
        snprintf(types.types[types.count++], STLXWIN_MIME_MAX, "%s", names[i]);
    }

    return types;
}

static void check_kinds(void) {
    const char *mime = NULL;
    stlxwin_offer_types files = types_of("text/plain;charset=utf-8", "text/uri-list");
    check(drop_kind(&files, &mime) == DROP_FILES && strcmp(mime, "text/uri-list") == 0,
          "a URI list wins over the same paths as text");

    stlxwin_offer_types text = types_of("text/html", "text/plain;charset=utf-8");
    check(drop_kind(&text, &mime) == DROP_TEXT && strcmp(mime, "text/plain;charset=utf-8") == 0,
          "text reads as UTF-8 text");

    stlxwin_offer_types plain = types_of("text/plain", NULL);
    check(drop_kind(&plain, &mime) == DROP_TEXT && strcmp(mime, "text/plain") == 0, "plain text reads too");

    stlxwin_offer_types image = types_of("image/png", "text/html");
    check(drop_kind(&image, &mime) == DROP_REFUSED && mime == NULL, "an image alone is refused");
}

static int quotes(const char *name, const char *expected) {
    char out[256];
    size_t n = drop_quote(name, strlen(name), out);
    return n == strlen(expected) && strcmp(out, expected) == 0;
}

static void check_quoting(void) {
    check(quotes("/stellux/userland/apps/stlxpkg/src/tar.c", "/stellux/userland/apps/stlxpkg/src/tar.c"),
          "a name of only safe bytes goes bare");
    check(quotes("/tmp/a_b@c%d+e=f:g,h.-", "/tmp/a_b@c%d+e=f:g,h.-"), "every byte of the bare set stays bare");
    check(quotes("/tmp/release notes.md", "'/tmp/release notes.md'"), "a space goes in single quotes");
    check(quotes("/tmp/a|b>c<d&e;f$g", "'/tmp/a|b>c<d&e;f$g'"), "what either shell reads goes in single quotes");

    check(quotes("/tmp/Albert's notes.txt", "\"/tmp/Albert's notes.txt\""),
          "a single quote goes in double quotes");
    check(quotes("/tmp/it's $5.txt", "'/tmp/it'\\''s $5.txt'"),
          "a single quote with a dollar takes POSIX's form");
    check(quotes("/tmp/it's \"x\"", "'/tmp/it'\\''s \"x\"'"), "a single quote with a double quote takes POSIX's form");
    check(quotes("/tmp/it's!", "'/tmp/it'\\''s!'"), "a single quote with a bang takes POSIX's form");

    char out[64];
    check(drop_quote("/tmp/two\nlines", 14, out) == 0, "a line break is refused");
    check(drop_quote("/tmp/tab\there", 13, out) == 0, "a tab is refused");
    check(drop_quote("", 0, out) == 0, "an empty name is refused");
}

static int pastes(const char *list, const char *expected) {
    drop_paste_t paste;
    int rc = drop_paste_uri_list(list, strlen(list), &paste);
    int holds = rc == 0 && paste.text && strcmp(paste.text, expected) == 0;
    free(paste.text);
    return holds;
}

static void check_uri_lists(void) {
    check(pastes("file:///etc/stlxdm/stlxdm.conf\r\n", "/etc/stlxdm/stlxdm.conf "),
          "a file URI pastes its path and a space");
    check(pastes("file://localhost/tmp/a%20b.txt\r\n", "'/tmp/a b.txt' "), "localhost is this computer");
    check(pastes("file:///tmp/caf%C3%A9.txt", "'/tmp/caf\xc3\xa9.txt' "), "escapes decode to their bytes");
    check(pastes("file:///tmp/100%.txt", "/tmp/100%.txt "), "a percent that escapes nothing stays");
    check(pastes("# from File Manager\r\nfile:///a\r\n\r\nfile:///b c\n", "/a '/b c' "),
          "comments and empty lines are skipped, both line endings read");
    check(pastes("https://example.com/a?b=1&c=2", "'https://example.com/a?b=1&c=2' "),
          "a URI of another scheme pastes as written, quoted");

    drop_paste_t paste;
    const char *mixed = "file://devbox/home/a.txt\r\nfile:///tmp/x\r\nfile://other/b\r\nfile:///tmp/bad%0Aname\r\n";

    check(drop_paste_uri_list(mixed, strlen(mixed), &paste) == 0, "a mixed list reads");
    check(paste.pasted == 1 && strcmp(paste.text, "/tmp/x ") == 0, "only this computer's safe files paste");
    check(paste.remote == 2 && strcmp(paste.host, "devbox") == 0, "other computers are counted and the first named");
    check(paste.unsafe == 1, "a decoded line break leaves its file out");
    free(paste.text);

    check(drop_paste_uri_list("", 0, &paste) == 0 && paste.pasted == 0 && paste.text == NULL,
          "an empty list pastes nothing");
}

int main(void) {
    check_kinds();
    check_quoting();
    check_uri_lists();

    if (g_failures != 0) {
        printf("drop: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("drop: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
