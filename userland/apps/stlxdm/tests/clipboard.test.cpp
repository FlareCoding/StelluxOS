/* clipboard, run on the build machine: an offer reads only printable types with their flags and reserved fields 0, an
 * item finds a representation by its exact type, and an item closes its objects once replaced */

#include "clipboard.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <unistd.h>
#include <utility>

constexpr uint32_t MAX_REPORTS = 8;

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

static swp_offer offer_of(std::initializer_list<std::pair<const char*, uint64_t>> types) {
    swp_offer offer = {};
    for (const auto& [mime, len] : types) {
        swp_offer_type& type = offer.types[offer.count++];
        snprintf(type.mime, sizeof(type.mime), "%s", mime);
        type.len = len;
    }

    return offer;
}

static bool is_open(int object) {
    return fcntl(object, F_GETFD) != -1;
}

static void check_offers() {
    std::vector<clipboard_rep> reps;
    check(read_offer(offer_of({ { "text/uri-list", 30 }, { SWP_MIME_TEXT, 23 } }), &reps),
          "an offer of two named representations reads");
    check(reps.size() == 2 && reps[0].mime_type == "text/uri-list" && reps[0].len == 30 &&
          reps[1].mime_type == SWP_MIME_TEXT && reps[1].len == 23,
          "an offer's representations keep their order, types and lengths");
    check(reps[0].object == -1 && reps[1].object == -1, "reading an offer takes no object");

    check(read_offer(offer_of({}), &reps) && reps.empty(), "an offer of nothing empties the clipboard");
    check(read_offer(offer_of({ { SWP_MIME_TEXT, 0 } }), &reps) && reps.size() == 1 && reps[0].len == 0,
          "a representation without bytes reads");
    check(!read_offer(offer_of({ { "", 4 } }), &reps), "an unnamed representation is refused");
    check(!read_offer(offer_of({ { "text/pl\x01" "ain", 4 } }), &reps), "a type that is not printable is refused");

    swp_offer unterminated = offer_of({ { "text/html", 8 } });
    memset(unterminated.types[0].mime, 'a', sizeof(unterminated.types[0].mime));
    check(!read_offer(unterminated, &reps), "a type without its NUL is refused");

    swp_offer flagged = offer_of({ { "image/png", 8 } });
    flagged.types[0].flags = 1;
    check(!read_offer(flagged, &reps), "a representation's flags must be 0");

    swp_offer reserved = offer_of({ { "image/png", 8 } });
    reserved.types[0].reserved = 1;
    check(!read_offer(reserved, &reps), "a representation's reserved field must be 0");

    swp_offer crowded = offer_of({ { "text/html", 8 } });
    crowded.count = SWP_OFFER_TYPES_MAX + 1;
    check(!read_offer(crowded, &reps), "more representations than an offer carries are refused");
}

static void check_items() {
    int text_pipe[2];
    int list_pipe[2];
    if (pipe(text_pipe) != 0 || pipe(list_pipe) != 0) {
        check(false, "the test makes its objects");
        return;
    }

    clipboard_item item({ { SWP_MIME_TEXT, 5, text_pipe[0] }, { "text/uri-list", 9, list_pipe[0] } },
                        SWP_CLIPBOARD_PRIVATE);
    const clipboard_rep* text = item.find(SWP_MIME_TEXT);
    check(text && text->object == text_pipe[0] && text->len == 5, "an item finds its text");
    check(!item.find("text/plain"), "a type matches exactly, its parameters included");
    check(!item.find("image/png"), "an item has no type it was not offered");
    check(item.flags() == SWP_CLIPBOARD_PRIVATE, "an item keeps its flags");

    clipboard_item kept = std::move(item);
    check(kept.reps().size() == 2 && is_open(text_pipe[0]) && is_open(list_pipe[0]),
          "a moved item hands over its objects open");

    kept = clipboard_item();
    check(kept.reps().empty() && !is_open(text_pipe[0]) && !is_open(list_pipe[0]),
          "a replaced item closes its objects");

    close(text_pipe[1]);
    close(list_pipe[1]);
}

static void check_types() {
    swp_clipboard_types empty = describe_types(clipboard_item());
    check(empty.count == 0 && empty.types[0][0] == '\0', "an empty clipboard tells no types");

    clipboard_item item({ { "image/png", 12, -1 }, { SWP_MIME_TEXT, 3, -1 } }, 0);
    swp_clipboard_types types = describe_types(item);
    check(types.count == 2 && strcmp(types.types[0], "image/png") == 0 && strcmp(types.types[1], SWP_MIME_TEXT) == 0,
          "an item tells its types in the offer's order");
    check(types.types[2][0] == '\0' && types.types[3][0] == '\0', "the types past the count stay zero");
}

static void check_uri_lists() {
    check(file_uri_list("/etc/stlxdm/stlxdm.conf") == "file:///etc/stlxdm/stlxdm.conf\r\n",
          "a plain path names its file as it is");
    check(file_uri_list("/tmp/a b%c#d") == "file:///tmp/a%20b%25c%23d\r\n",
          "spaces, percents and hashes are escaped");
    check(file_uri_list("/home/caf\xc3\xa9.txt") == "file:///home/caf%C3%A9.txt\r\n",
          "UTF-8 bytes are escaped one by one");
}

int main() {
    check_offers();
    check_items();
    check_types();
    check_uri_lists();

    if (g_failures != 0) {
        printf("clipboard: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("clipboard: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
