/* clipboard_search, run on the build machine over scratch files: a query is found in any case across a chunk's end and
 * chunks in, a text without it is left out, nothing stale follows the newest answer, and duplicates are closed */

#include "shell/clipboard_search.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;
constexpr int WAIT_MS = 5000;
constexpr int LATE_WAIT_MS = 200;
constexpr int POLL_MS = 10;
constexpr size_t CHUNK_BYTES = 1024 * 1024;
constexpr size_t LONG_TEXT_BYTES = 3 * CHUNK_BYTES;

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

/* A scratch file holding `text`, unlinked at once so only its descriptor names it */
static int scratch_object(const std::string& text) {
    char path[] = "/tmp/stlxdm-search-XXXXXX";
    int object = mkstemp(path);
    if (object < 0) {
        check(false, "the test makes its scratch file");
        return -1;
    }

    unlink(path);
    check(write(object, text.data(), text.size()) == static_cast<ssize_t>(text.size()),
          "the test writes its scratch file");

    return object;
}

static std::string long_text_with(const char* needle, size_t at) {
    std::string text(LONG_TEXT_BYTES, 'a');
    text.replace(at, std::string(needle).size(), needle);
    return text;
}

/* Takes the searcher's answers until one for `request` arrives or `limit_ms` run out */
static bool answered(clipboard_search& search, uint64_t request, text_matches& matches, int limit_ms = WAIT_MS) {
    for (int waited = 0; waited < limit_ms; waited += POLL_MS) {
        pollfd pfd = { search.wake_fd(), POLLIN, 0 };
        poll(&pfd, 1, POLL_MS);
        if (search.take(&matches) && (request == 0 || matches.request == request)) {
            return true;
        }
    }

    return false;
}

static void check_search(clipboard_search& search, const std::vector<int>& objects) {
    std::vector<int> duplicates;
    std::vector<searched_text> texts;
    for (size_t i = 0; i < objects.size(); i++) {
        duplicates.push_back(dup(objects[i]));
        texts.emplace_back(i + 1, duplicates.back(), static_cast<uint64_t>(lseek(objects[i], 0, SEEK_END)));
    }

    text_matches matches;
    uint64_t request = search.search("needlex", std::move(texts));
    check(answered(search, request, matches), "a search is answered");
    check(matches.ids == std::vector<uint64_t>{ 1, 2, 4 },
          "a search finds its query across a chunk's end, chunks in and in any case, and leaves out a text without it");

    bool are_closed = true;
    for (int duplicate : duplicates) {
        are_closed = are_closed && fcntl(duplicate, F_GETFD) == -1;
    }

    check(are_closed, "an answered search has closed the duplicates it was given");
}

static void check_newest(clipboard_search& search, const std::vector<int>& objects) {
    std::vector<searched_text> older;
    std::vector<searched_text> newer;
    for (size_t i = 0; i < objects.size(); i++) {
        uint64_t len = static_cast<uint64_t>(lseek(objects[i], 0, SEEK_END));
        older.emplace_back(i + 1, dup(objects[i]), len);
        newer.emplace_back(i + 1, dup(objects[i]), len);
    }

    search.search("needlex", std::move(older));
    uint64_t request = search.search("absent", std::move(newer));

    text_matches matches;
    check(answered(search, request, matches) && matches.ids.empty(), "the newest request is answered");
    check(!answered(search, 0, matches, LATE_WAIT_MS), "no older answer follows the newest");
}

int main() {
    clipboard_search idle;
    check(idle.search("needlex", {}) == 0, "a searcher not started takes no request");

    clipboard_search search;
    check(search.start(), "the searcher starts");

    std::vector<int> objects = { scratch_object(long_text_with("NeedleX", CHUNK_BYTES - 3)),
                                 scratch_object(long_text_with("NEEDLEX", 5 * CHUNK_BYTES / 2)),
                                 scratch_object(long_text_with("needle", CHUNK_BYTES)),
                                 scratch_object("a short needlex") };
    check_search(search, objects);
    check_newest(search, objects);

    for (int object : objects) {
        close(object);
    }

    if (g_failures != 0) {
        printf("clipboard_search: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("clipboard_search: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
