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
#include <vector>

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

static void check_png_sizes() {
    const char header[] = "\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\x02\xf6\0\0\x01\x4a";
    uint32_t width = 0;
    uint32_t height = 0;
    check(png_size(std::string_view(header, 24), &width, &height) && width == 758 && height == 330,
          "a PNG's header gives its size");
    check(!png_size("GIF89a not a png header....", &width, &height), "other bytes give no size");
}

static void check_reads() {
    char path[] = "/tmp/stlxdm-clipboard-XXXXXX";
    int object = mkstemp(path);
    const char text[] = "first line\nsecond line";
    bool is_written = object >= 0 && write(object, text, sizeof(text) - 1) == static_cast<ssize_t>(sizeof(text) - 1);
    check(is_written, "the test writes its object");

    clipboard_rep rep = { SWP_MIME_TEXT, sizeof(text) - 1, object };
    check(read_bytes(rep, 1024) == text, "a read takes the whole representation");
    check(read_bytes(rep, 5) == "first", "a read stops at its limit");

    clipboard_rep closed = { SWP_MIME_TEXT, 4, -1 };
    check(read_bytes(closed, 1024).empty(), "a representation without an object reads as empty");

    close(object);
    unlink(path);
}

static void check_uri_lists() {
    std::string list = file_uri_list("/tmp/a b%c") + "# a comment\r\nhttps://example.com/x\r\nfile:///etc/hosts\n";
    check(uri_list_paths(list) == std::vector<std::string>{ "/tmp/a b%c", "/etc/hosts" },
          "a uri list's local files read back as paths, its comments and web addresses left out");

    check(file_uri_list("/etc/stlxdm/stlxdm.conf") == "file:///etc/stlxdm/stlxdm.conf\r\n",
          "a plain path names its file as it is");
    check(file_uri_list("/tmp/a b%c#d") == "file:///tmp/a%20b%25c%23d\r\n",
          "spaces, percents and hashes are escaped");
    check(file_uri_list("/home/caf\xc3\xa9.txt") == "file:///home/caf%C3%A9.txt\r\n",
          "UTF-8 bytes are escaped one by one");
}

/* A copy of `len` bytes of text, its object a pipe's end, which tells when it closes */
static clipboard_item text_item(uint64_t len, uint32_t flags, int* object) {
    int ends[2];
    if (pipe(ends) != 0) {
        return clipboard_item();
    }

    close(ends[1]);
    if (object) {
        *object = ends[0];
    }

    return clipboard_item({ { SWP_MIME_TEXT, len, ends[0] } }, flags);
}

/* Copies of one size count as the same content here */
static bool same_size(const clipboard_item& a, const clipboard_item& b) {
    return a.size() == b.size();
}

static std::vector<uint64_t> listed_sizes(const clipboard_history& history) {
    std::vector<uint64_t> sizes;
    for (const clipboard_entry* entry : history.entries()) {
        sizes.push_back(entry->item.size());
    }

    return sizes;
}

static const clipboard_entry* entry_of_size(const clipboard_history& history, uint64_t size) {
    for (const clipboard_entry* entry : history.entries()) {
        if (entry->item.size() == size) {
            return entry;
        }
    }

    return nullptr;
}

static void check_history_order() {
    clipboard_history history(same_size);
    history.set_limits(10, 1000);
    for (uint64_t len : { 1, 2, 3 }) {
        history.copy(text_item(len, 0, nullptr), "Terminal", static_cast<int64_t>(len));
    }

    check(history.current().size() == 3, "the newest copy is current");
    check(listed_sizes(history) == std::vector<uint64_t>{ 3, 2, 1 }, "entries list newest first");
    check(history.kept_count() == 2 && history.kept_bytes() == 3, "the budget counts every entry but the current one");

    int private_object = -1;
    history.copy(text_item(4, SWP_CLIPBOARD_PRIVATE, &private_object), "Passwords", 4);
    check(history.current().size() == 4 && listed_sizes(history) == std::vector<uint64_t>{ 3, 2, 1 },
          "a private copy is current but unlisted");

    history.copy(text_item(5, 0, nullptr), "Terminal", 5);
    check(!is_open(private_object) && listed_sizes(history) == std::vector<uint64_t>{ 5, 3, 2, 1 },
          "a private copy leaves no entry once replaced");

    int repeated_object = entry_of_size(history, 2)->item.reps()[0].object;
    history.copy(text_item(2, 0, nullptr), "Settings", 6);
    check(!is_open(repeated_object) && listed_sizes(history) == std::vector<uint64_t>{ 2, 5, 3, 1 },
          "the same content copied again moves its entry up");
}

static void check_history_budget() {
    clipboard_history history(same_size);
    history.set_limits(2, 1000);
    int oldest_object = -1;
    history.copy(text_item(1, 0, &oldest_object), "Terminal", 1);
    for (uint64_t len : { 2, 3, 4 }) {
        history.copy(text_item(len, 0, nullptr), "Terminal", static_cast<int64_t>(len));
    }

    check(!is_open(oldest_object) && listed_sizes(history) == std::vector<uint64_t>{ 4, 3, 2 },
          "past the item limit the oldest entry leaves");

    history.set_pinned(entry_of_size(history, 2)->id, true);
    history.copy(text_item(5, 0, nullptr), "Terminal", 5);
    check(listed_sizes(history) == std::vector<uint64_t>{ 2, 5, 4 },
          "a pinned entry stays and the oldest unpinned one leaves instead");

    clipboard_history bytes(same_size);
    bytes.set_limits(10, 100);
    for (uint64_t len : { 60, 50, 40 }) {
        bytes.copy(text_item(len, 0, nullptr), "Sheen", static_cast<int64_t>(len));
    }

    check(listed_sizes(bytes) == std::vector<uint64_t>{ 40, 50 }, "past the byte limit the oldest entry leaves");

    int huge_object = -1;
    bytes.copy(text_item(500, 0, &huge_object), "Sheen", 500);
    check(bytes.current().size() == 500 && listed_sizes(bytes) == std::vector<uint64_t>{ 500, 40, 50 },
          "a copy past the budget stays while it is current");

    bytes.copy(text_item(10, 0, nullptr), "Sheen", 510);
    check(!is_open(huge_object) && listed_sizes(bytes) == std::vector<uint64_t>{ 10, 40, 50 },
          "a copy past the budget leaves once replaced");
}

static void check_history_actions() {
    clipboard_history history(same_size);
    history.set_limits(10, 1000);
    for (uint64_t len : { 1, 2, 3 }) {
        history.copy(text_item(len, 0, nullptr), "Terminal", static_cast<int64_t>(len));
    }

    uint64_t two = entry_of_size(history, 2)->id;
    history.make_current(two, 9);
    check(history.current().size() == 2 && history.is_current(two) && history.find(two)->copied_ns == 9 &&
          listed_sizes(history) == std::vector<uint64_t>{ 2, 3, 1 },
          "an entry made current moves up as a fresh copy");

    history.remove(two);
    check(history.current().empty() && listed_sizes(history) == std::vector<uint64_t>{ 3, 1 },
          "removing the current entry empties the clipboard");

    history.set_pinned(entry_of_size(history, 1)->id, true);
    history.copy(text_item(4, 0, nullptr), "Terminal", 10);
    size_t cleared = history.clear_unpinned();
    check(cleared == 2 && history.current().empty() && listed_sizes(history) == std::vector<uint64_t>{ 1 },
          "clearing keeps the pinned entries and empties an unpinned clipboard");
}

int main() {
    check_offers();
    check_items();
    check_types();
    check_reads();
    check_uri_lists();
    check_png_sizes();

    check_history_order();
    check_history_budget();
    check_history_actions();

    if (g_failures != 0) {
        printf("clipboard: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("clipboard: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
