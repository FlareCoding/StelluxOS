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

    clipboard_item item(make_reps({ { SWP_MIME_TEXT, 5, text_pipe[0] }, { "text/uri-list", 9, list_pipe[0] } }),
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

    int moved_pipe[2];
    check(pipe(moved_pipe) == 0, "the test makes another object");
    clipboard_rep first(SWP_MIME_TEXT, 1, moved_pipe[0]);
    clipboard_rep second = std::move(first);
    check(first.object == -1 && second.object == moved_pipe[0], "a moved representation hands over its object");

    second = clipboard_rep();
    check(!is_open(moved_pipe[0]), "a representation closes its object when it is replaced, and only once");
    close(moved_pipe[1]);

    close(text_pipe[1]);
    close(list_pipe[1]);
}

static void check_types() {
    swp_clipboard_types empty = describe_types(clipboard_item());
    check(empty.count == 0 && empty.types[0][0] == '\0', "an empty clipboard tells no types");

    clipboard_item item(make_reps({ { "image/png", 12, -1 }, { SWP_MIME_TEXT, 3, -1 } }), 0);
    swp_clipboard_types types = describe_types(item);
    check(types.count == 2 && strcmp(types.types[0], "image/png") == 0 && strcmp(types.types[1], SWP_MIME_TEXT) == 0,
          "an item tells its types in the offer's order");
    check(types.types[2][0] == '\0' && types.types[3][0] == '\0', "the types past the count stay zero");
}

/* Text answers to any of text's names and is told by one, so a client asking for UTF-8 text reads a bare text/plain */
static void check_text_aliases() {
    check(is_text_type("text/plain") && is_text_type(SWP_MIME_TEXT) && is_text_type("Text/Plain; charset=UTF-8"),
          "text/plain with or without its UTF-8 charset names text, in any case and spacing");
    check(!is_text_type("text/html") && !is_text_type("text/plain;charset=latin1"), "other types are not text");

    clipboard_item bare(make_reps({ { "text/plain", 5, -1 } }), 0);
    swp_clipboard_types told = describe_types(bare);
    check(told.count == 1 && strcmp(told.types[0], SWP_MIME_TEXT) == 0, "a bare text/plain is told as UTF-8 text");
    check(bare.find_answer(SWP_MIME_TEXT) == &bare.reps()[0], "an ask for UTF-8 text reads a bare text/plain");
    check(!bare.find_answer("text/html"), "an ask for another type finds no text");

    clipboard_item both(make_reps({ { "text/html", 9, -1 }, { SWP_MIME_TEXT, 3, -1 }, { "text/plain", 3, -1 } }), 0);
    told = describe_types(both);
    check(told.count == 2 && strcmp(told.types[0], "text/html") == 0 && strcmp(told.types[1], SWP_MIME_TEXT) == 0,
          "text under two names is told once");
    check(both.find_answer("text/plain") == &both.reps()[2] && both.find_answer(SWP_MIME_TEXT) == &both.reps()[1],
          "an ask for a name the item has reads that representation");
}

static void check_png_sizes() {
    const char header[] = "\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\x02\xf6\0\0\x01\x4a";
    uint32_t width = 0;
    uint32_t height = 0;
    check(png_size(std::string_view(header, 24), &width, &height) && width == 758 && height == 330,
          "a PNG's header gives its size");
    check(!png_size("GIF89a not a png header....", &width, &height), "other bytes give no size");

    check(fits_preview(1920, 1080) && fits_preview(5120, 2880), "a capture of a 5K display or less previews");
    check(!fits_preview(16384, 16384) && !fits_preview(100000, 1), "a header promising too many pixels never decodes");
    check(!fits_preview(0, 0) && !fits_preview(640, 0), "a header without pixels never decodes");
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

    unlink(path);
}

static void check_folded_finds() {
    const std::string_view text = "git Pull origin, then PULL again \xC3\x89t\xC3\xA9";
    check(find_folded(text, "pull") == 4, "a query finds its first occurrence in any ASCII case");
    check(find_folded(text, "pull", 5) == 22, "a search from past the first occurrence finds the next");
    check(find_folded(text, "push") == std::string_view::npos, "a query the text lacks is not found");

    check(find_folded(text, "") == std::string_view::npos, "an empty query is never found");
    check(find_folded("pul", "pull") == std::string_view::npos, "a query longer than the text is not found");
    check(find_folded(text, "\xC3\x89t\xC3\xA9") == 33, "bytes past ASCII match exactly");
    check(find_folded(text, "\xC3\xA9t\xC3\xA9") == std::string_view::npos, "bytes past ASCII match in no other case");
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

    return clipboard_item(make_reps({ { SWP_MIME_TEXT, len, ends[0] } }), flags);
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

    const clipboard_entry* repeated = entry_of_size(history, 2);
    uint64_t repeated_id = repeated->id;
    int repeated_object = repeated->item.reps()[0].object;
    int recopied_object = -1;
    history.copy(text_item(2, 0, &recopied_object), "Settings", 6);

    check(listed_sizes(history) == std::vector<uint64_t>{ 2, 5, 3, 1 }, "the same content copied again moves its entry up");
    check(history.is_current(repeated_id) && is_open(repeated_object) && !is_open(recopied_object),
          "the moved entry keeps its id and objects, and the new copy's objects close");

    const clipboard_entry* moved = history.find(repeated_id);
    check(moved && moved->label == "Settings" && moved->copied_ns == 6,
          "the moved entry names the new copy's source and time");
}

/* A regular file holding `text`, which reads and maps as a shared memory object does */
static int object_holding(std::string_view text) {
    char path[] = "/tmp/stlxdm-clipboard-XXXXXX";
    int object = mkstemp(path);
    if (object < 0) {
        return -1;
    }

    unlink(path);
    if (write(object, text.data(), text.size()) != static_cast<ssize_t>(text.size())) {
        close(object);
        return -1;
    }

    return object;
}

static clipboard_item item_holding(std::string_view text, const char* mime_type = SWP_MIME_TEXT) {
    return clipboard_item(make_reps({ { mime_type, text.size(), object_holding(text) } }), 0);
}

static void check_same_content() {
    clipboard_item hello = item_holding("hello");
    check(same_content(hello, item_holding("hello")), "items of one type holding the same bytes match");
    check(!same_content(hello, item_holding("jello")), "items whose bytes differ at the same length differ");
    check(!same_content(hello, item_holding("hello!")), "items of different lengths differ");
    check(!same_content(hello, item_holding("hello", "text/html")), "items of different types differ");

    int ends[2];
    if (pipe(ends) != 0) {
        check(false, "the test makes its pipe");
        return;
    }

    close(ends[1]);
    clipboard_item piped(make_reps({ { SWP_MIME_TEXT, 5, ends[0] } }), 0);
    check(!same_content(hello, piped), "an object that refuses its mapping counts as different");
}

static clipboard_item fingerprinted(const std::string& text) {
    clipboard_item item = item_holding(text);
    item.take_fingerprints();
    return item;
}

/* A fingerprint covers a short text whole and a long one by spread samples, and only tells unlike items apart, so a
 * difference between samples still reaches the bytes */
static void check_fingerprints() {
    std::string sample(1 << 20, 'a');
    int first = object_holding(sample);
    int second = object_holding(sample);

    check(sampled_fingerprint(first, sample.size()) == sampled_fingerprint(second, sample.size()),
          "the same bytes give the same fingerprint");
    check(sampled_fingerprint(first, sample.size()) != sampled_fingerprint(first, sample.size() - 1),
          "a different length gives a different fingerprint");
    close(first);
    close(second);

    std::string short_text(10000, 'b');
    std::string short_changed = short_text;
    short_changed[5000] = 'c';

    int whole = object_holding(short_text);
    int changed = object_holding(short_changed);
    check(sampled_fingerprint(whole, short_text.size()) != sampled_fingerprint(changed, short_changed.size()),
          "a text up to 64 KB is fingerprinted whole");
    close(whole);
    close(changed);

    std::string at_start = sample;
    at_start[0] = 'z';
    check(!same_content(fingerprinted(sample), fingerprinted(at_start)), "items whose samples differ are unlike");

    std::string between = sample;
    between[100 * 1024 + 2048] = 'z';
    clipboard_item kept = fingerprinted(sample);
    clipboard_item recopy = fingerprinted(between);
    check(kept.reps()[0].fingerprint == recopy.reps()[0].fingerprint && !same_content(kept, recopy),
          "items alike in their samples are still compared byte by byte");
    check(same_content(kept, fingerprinted(sample)), "fingerprinted items holding the same bytes match");
}

/* The server's own matcher, so only the same bytes merge entries */
static void check_history_recopies() {
    clipboard_history history(same_content);
    history.set_limits(10, 1000);
    history.copy(item_holding("first"), "Terminal", 1);
    uint64_t first = history.entries().front()->id;
    history.set_pinned(first, true);

    history.copy(item_holding("other"), "Terminal", 2);
    check(history.entries().size() == 2, "a copy of the same length but other bytes gets its own entry");

    history.copy(item_holding("first"), "Settings", 3);
    check(history.entries().size() == 2 && history.is_current(first), "a pinned entry copied again keeps its id");
    check(history.find(first) && history.find(first)->pinned, "a pinned entry copied again stays pinned");
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
    check(listed_sizes(history) == std::vector<uint64_t>{ 2, 5, 4, 3 },
          "a pinned entry stays outside the item limit, which the unpinned ones fill");

    history.copy(text_item(6, 0, nullptr), "Terminal", 6);
    check(listed_sizes(history) == std::vector<uint64_t>{ 2, 6, 5, 4 },
          "past the item limit the oldest unpinned entry leaves");

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

/* Pins keep a budget of their own, so pinned entries never push unpinned ones out */
static void check_history_pins() {
    clipboard_history bytes(same_size);
    bytes.set_limits(10, 100);
    for (uint64_t len : { 30, 31, 32 }) {
        bytes.copy(text_item(len, 0, nullptr), "Settings", static_cast<int64_t>(len));
        bytes.set_pinned(entry_of_size(bytes, len)->id, true);
    }

    for (uint64_t len : { 1, 2, 3, 4, 5 }) {
        bytes.copy(text_item(len, 0, nullptr), "Terminal", static_cast<int64_t>(100 + len));
    }

    check(listed_sizes(bytes) == std::vector<uint64_t>{ 32, 31, 30, 5, 4, 3, 2, 1 },
          "pins holding most of the byte limit leave the unpinned entries a whole budget");
    check(bytes.kept_bytes() == 10 && bytes.pinned_bytes() == 93 && bytes.pinned_count() == 3,
          "each budget counts only its own entries");

    clipboard_history count(same_size);
    count.set_limits(2, 1000);
    for (uint64_t len : { 10, 11 }) {
        count.copy(text_item(len, 0, nullptr), "Settings", static_cast<int64_t>(len));
        count.set_pinned(entry_of_size(count, len)->id, true);
    }

    for (uint64_t len : { 1, 2, 3 }) {
        count.copy(text_item(len, 0, nullptr), "Terminal", static_cast<int64_t>(100 + len));
    }

    check(listed_sizes(count) == std::vector<uint64_t>{ 11, 10, 3, 2, 1 },
          "pins filling the item limit leave the unpinned entries a whole limit");
}

/* The budgets count what an object holds, so a length shorter than its object hides nothing */
static void check_held_budget() {
    int ends[2];
    if (pipe(ends) != 0) {
        check(false, "the test makes its pipe");
        return;
    }

    close(ends[1]);
    clipboard_history history(same_size);
    history.set_limits(10, 100);
    history.copy(clipboard_item(make_reps({ { SWP_MIME_TEXT, 1, ends[0], 500 } }), 0), "Sheen", 1);
    check(history.current().size() == 1 && history.current().held() == 500,
          "an item counts what its objects hold beside the lengths they claim");

    history.copy(text_item(2, 0, nullptr), "Sheen", 2);
    check(!is_open(ends[0]) && listed_sizes(history) == std::vector<uint64_t>{ 2 },
          "an object past the budget leaves once replaced, whatever length it claimed");
}

/* A pin past the pins' own budget is refused, and an unpin makes room */
static void check_pin_limits() {
    clipboard_history history(same_size);
    history.set_limits(2, 100);
    history.copy(text_item(60, 0, nullptr), "Sheen", 1);
    uint64_t sixty = entry_of_size(history, 60)->id;
    check(history.can_pin(sixty) && history.set_pinned(sixty, true), "a pin within the pins' budget goes through");

    history.copy(text_item(50, 0, nullptr), "Sheen", 2);
    uint64_t fifty = entry_of_size(history, 50)->id;
    check(!history.can_pin(fifty) && !history.set_pinned(fifty, true) && !history.find(fifty)->pinned,
          "a pin past the pins' byte limit is refused");
    check(history.set_pinned(sixty, false) && history.set_pinned(fifty, true), "an unpin makes room for another pin");

    history.copy(text_item(1, 0, nullptr), "Sheen", 3);
    history.copy(text_item(2, 0, nullptr), "Sheen", 4);
    uint64_t one = entry_of_size(history, 1)->id;
    uint64_t two = entry_of_size(history, 2)->id;
    check(history.set_pinned(one, true) && !history.set_pinned(two, true), "a pin past the pins' item limit is refused");
    check(!history.can_pin(12345) && !history.set_pinned(12345, true), "an entry the history lacks cannot be pinned");
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

static void check_history_discards() {
    clipboard_history history(same_size);
    history.set_limits(10, 1000);
    history.copy(text_item(1, 0, nullptr), "Terminal", 1);
    uint64_t replaced = history.current_id();

    history.copy(text_item(2, 0, nullptr), "Screenshot", 2);
    history.set_name(history.current_id(), "Region");
    check(history.find(history.current_id())->name == "Region", "a capture is named by its kind");
    check(history.restores_on_discard(history.current_id()), "the current capture's Discard would put a copy back");
    history.discard(history.current_id());
    check(history.is_current(replaced) && history.find(replaced)->copied_ns == 1 &&
              listed_sizes(history) == std::vector<uint64_t>{ 1 },
          "discarding the current capture puts back the copy it replaced as it was");

    history.copy(text_item(3, 0, nullptr), "Screenshot", 3);
    uint64_t capture = history.current_id();
    history.copy(text_item(4, 0, nullptr), "Terminal", 4);
    check(!history.restores_on_discard(capture), "a capture no longer current would put nothing back");
    history.discard(capture);
    check(history.current().size() == 4 && listed_sizes(history) == std::vector<uint64_t>{ 4, 1 },
          "discarding a capture no longer current leaves the clipboard as it is");

    history.copy(text_item(5, SWP_CLIPBOARD_PRIVATE, nullptr), "Passwords", 5);
    check(history.current_id() == 0, "a private copy has no id");

    history.copy(text_item(6, 0, nullptr), "Screenshot", 6);
    check(!history.restores_on_discard(history.current_id()), "a capture over a private copy would put nothing back");
    history.discard(history.current_id());
    check(history.current().empty() && listed_sizes(history) == std::vector<uint64_t>{ 4, 1 },
          "discarding a capture that replaced a private copy empties the clipboard, which the history knows itself");
}

/* Two stacked captures discarded in either order end where they began, and a recapture or a capture made current
 * again undoes to the copy it last replaced */
static void check_discard_chains() {
    for (bool is_oldest_first : { true, false }) {
        clipboard_history history(same_size);
        history.set_limits(10, 1000);
        history.copy(text_item(1, 0, nullptr), "Terminal", 1);
        uint64_t text = history.current_id();

        history.copy(text_item(2, 0, nullptr), "Screenshot", 2);
        uint64_t first = history.current_id();
        history.copy(text_item(3, 0, nullptr), "Screenshot", 3);
        uint64_t second = history.current_id();

        history.discard(is_oldest_first ? first : second);
        history.discard(is_oldest_first ? second : first);
        check(history.is_current(text) && listed_sizes(history) == std::vector<uint64_t>{ 1 },
              is_oldest_first ? "two captures discarded oldest first put back the copy before both"
                              : "two captures discarded newest first put back the copy before both");
    }

    clipboard_history recaptured(same_size);
    recaptured.set_limits(10, 1000);
    recaptured.copy(text_item(1, 0, nullptr), "Terminal", 1);
    uint64_t text = recaptured.current_id();

    recaptured.copy(text_item(2, 0, nullptr), "Screenshot", 2);
    recaptured.copy(text_item(2, 0, nullptr), "Screenshot", 3);
    recaptured.discard(recaptured.current_id());
    check(recaptured.is_current(text) && listed_sizes(recaptured) == std::vector<uint64_t>{ 1 },
          "discarding an identical recapture puts back the copy before both captures");

    clipboard_history again(same_size);
    again.set_limits(10, 1000);
    again.copy(text_item(1, 0, nullptr), "Terminal", 1);
    again.copy(text_item(2, 0, nullptr), "Screenshot", 2);
    uint64_t capture = again.current_id();

    again.copy(text_item(3, 0, nullptr), "Terminal", 3);
    uint64_t later = again.current_id();

    again.make_current(capture, 4);
    again.discard(capture);
    check(again.is_current(later), "discarding a capture made current again puts back the copy it last replaced");
}

/* Discard follows the order entries became current, so a capture made current again, an identical recapture of a
 * capture no longer current, and a capture after the clipboard emptied each undo to what was current before them */
static void check_discard_order() {
    for (bool is_chosen_first : { true, false }) {
        clipboard_history history(same_size);
        history.set_limits(10, 1000);
        history.copy(text_item(1, 0, nullptr), "Terminal", 1);
        uint64_t text = history.current_id();

        history.copy(text_item(2, 0, nullptr), "Screenshot", 2);
        uint64_t chosen = history.current_id();
        history.copy(text_item(3, 0, nullptr), "Screenshot", 3);
        uint64_t other = history.current_id();
        history.make_current(chosen, 4);

        history.discard(is_chosen_first ? chosen : other);
        check(history.is_current(is_chosen_first ? other : chosen),
              is_chosen_first ? "discarding a capture made current again puts back the capture it displaced"
                              : "discarding the displaced capture leaves the one made current again");

        history.discard(is_chosen_first ? other : chosen);
        check(history.is_current(text) && listed_sizes(history) == std::vector<uint64_t>{ 1 },
              is_chosen_first ? "then discarding the other capture puts back the copy before both"
                              : "then discarding the chosen capture puts back the copy before both");
    }

    clipboard_history recaptured(same_size);
    recaptured.set_limits(10, 1000);
    recaptured.copy(text_item(1, 0, nullptr), "Terminal", 1);

    recaptured.copy(text_item(2, 0, nullptr), "Screenshot", 2);
    uint64_t first = recaptured.current_id();
    recaptured.copy(text_item(3, 0, nullptr), "Terminal", 3);
    uint64_t later = recaptured.current_id();

    recaptured.copy(text_item(2, 0, nullptr), "Screenshot", 4);
    check(recaptured.current_id() == first, "an identical recapture of an older capture moves its entry up by its id");

    recaptured.discard(first);
    check(recaptured.is_current(later) && recaptured.find(later)->copied_ns == 3 &&
              listed_sizes(recaptured) == std::vector<uint64_t>{ 3, 1 },
          "discarding the recapture puts back the copy made between the two captures");

    recaptured.discard(first);
    check(recaptured.is_current(later), "the first capture's Discard then finds nothing left to undo");

    clipboard_history emptied(same_size);
    emptied.set_limits(10, 1000);
    emptied.copy(text_item(1, 0, nullptr), "Terminal", 1);
    emptied.copy(text_item(2, 0, nullptr), "Terminal", 2);
    emptied.remove(emptied.current_id());

    emptied.copy(text_item(3, 0, nullptr), "Screenshot", 3);
    emptied.discard(emptied.current_id());
    check(emptied.current().empty() && listed_sizes(emptied) == std::vector<uint64_t>{ 1 },
          "discarding a capture made after the current copy was deleted leaves the clipboard empty");
}

int main() {
    check_offers();
    check_items();
    check_types();
    check_text_aliases();
    check_reads();

    check_folded_finds();
    check_uri_lists();
    check_png_sizes();
    check_same_content();
    check_fingerprints();

    check_history_order();
    check_history_recopies();
    check_history_budget();
    check_history_actions();

    check_history_discards();
    check_discard_chains();
    check_discard_order();

    check_history_pins();
    check_held_budget();
    check_pin_limits();

    if (g_failures != 0) {
        printf("clipboard: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("clipboard: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
