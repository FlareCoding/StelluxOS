/* file_index, run on the build machine over a scratch tree: the walk leaves out skipped folders, hidden entries and
 * pipes, a search ranks names, a listing puts folders first with their previews, and the newest request wins */

#include "shell/file_index.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

constexpr uint32_t MAX_REPORTS = 8;
constexpr int WAIT_MS = 5000;
constexpr int LATE_WAIT_MS = 200;
constexpr int POLL_MS = 10;
constexpr size_t WALKED_ENTRIES = 9;

static uint64_t g_checks;
static uint64_t g_failures;
static char     g_root[] = "/tmp/stlxdm-index-XXXXXX";

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

static std::string at(const char* relative) {
    return std::string(g_root) + "/" + relative;
}

static void make_folder(const char* relative) {
    mkdir(at(relative).c_str(), 0755);
}

static void make_file(const char* relative, const char* text) {
    FILE* file = fopen(at(relative).c_str(), "w");
    if (!file) {
        check(false, relative);
        return;
    }

    fputs(text, file);
    fclose(file);
}

/* Takes what the index sends until `done` holds or `limit_ms` run out */
template <typename done_t>
static bool wait_until(file_index& index, file_answer& answer, walk_state& walk, done_t done,
                       int limit_ms = WAIT_MS) {
    for (int waited = 0; waited < limit_ms; waited += POLL_MS) {
        pollfd pfd = { index.wake_fd(), POLLIN, 0 };
        poll(&pfd, 1, POLL_MS);

        file_answer taken;
        if (index.take(&taken, &walk)) {
            answer = std::move(taken);
        }

        if (done()) {
            return true;
        }
    }

    return false;
}

static bool answered(file_index& index, uint64_t request, file_answer& answer) {
    walk_state walk;
    return wait_until(index, answer, walk, [&] { return answer.request == request; });
}

static void check_walk(file_index& index) {
    file_answer answer;
    walk_state walk;
    index.rewalk();
    bool walked = wait_until(index, answer, walk, [&] { return !walk.walking && walk.found > 0; });
    check(walked && walk.found == WALKED_ENTRIES, "the walk indexes every entry but skipped and hidden ones");
}

static void check_search(file_index& index) {
    file_answer answer;
    bool is_answered = answered(index, index.search("notes", 10), answer);
    check(is_answered && answer.total == 2, "a search finds each name the query matches");
    check(answer.files.size() == 2 && answer.files[0].name() == "notes.md", "a prefix ranks first");
    check(answer.files.size() == 2 && answer.files[1].name() == "sheen-notes.md", "a word start ranks next");
    check(answer.matches.size() == 2 && answer.matches[1].runs.size() == 1 && answer.matches[1].runs[0].start == 6,
          "a search marks the matched run of each name");

    check(answered(index, index.search("secret", 10), answer) && answer.total == 0, "hidden entries stay unindexed");
    check(answered(index, index.search("device", 10), answer) && answer.total == 0, "skipped folders stay unwalked");
}

static void check_listing(file_index& index) {
    file_answer answer;
    bool is_answered = answered(index, index.list(at("etc/stlxdm"), ""), answer);
    check(is_answered && answer.files.size() == 2, "a listing holds the folder's entries");
    check(answer.files.size() == 2 && answer.files[0].is_folder && answer.files[0].name() == "conf.d",
          "a listing puts folders first");

    bool is_previewed = answer.files.size() == 2 && answer.files[0].preview == "browser.conf, sheen.conf";
    check(is_previewed && answer.files[0].size == 2, "a listed folder counts and previews its entries");
    check(answer.files.size() == 2 && answer.files[1].size == 10, "a listed file has its size");
    const char* type = answer.files.size() == 2 ? answer.files[1].mime_type : nullptr;
    check(type && std::string_view(type) == "text/x-test", "the describer fills in each answered file");

    is_answered = answered(index, index.list(at("etc/stlxdm"), "ST"), answer);
    check(is_answered && answer.files.size() == 1 && answer.files[0].name() == "stlxdm.conf",
          "a listing keeps the names the prefix starts in any case");
    check(answer.matches.size() == 1 && answer.matches[0].runs.size() == 1 && answer.matches[0].runs[0].length == 2,
          "a listing marks the prefix");
}

static void check_newest_wins(file_index& index) {
    uint64_t older = index.search("notes", 10);
    uint64_t newer = index.search("conf", 10);

    file_answer answer;
    bool is_answered = answered(index, newer, answer);
    check(is_answered && answer.total == 4 && answer.files[0].name() == "conf.d", "the newest request is answered");

    walk_state walk;
    file_answer late;
    bool has_late = wait_until(index, late, walk, [&] { return late.request == older; }, LATE_WAIT_MS);
    check(older < newer && !has_late, "an older request's answer never follows a newer one's");
}

int main() {
    if (!mkdtemp(g_root)) {
        printf("file_index: cannot make a scratch directory\n");
        return 1;
    }

    for (const char* folder : { "etc", "etc/stlxdm", "etc/stlxdm/conf.d", "tmp", "dev", ".hidden" }) {
        make_folder(folder);
    }

    make_file("etc/stlxdm/stlxdm.conf", "[desktop]\n");
    make_file("etc/stlxdm/conf.d/browser.conf", "[taskbar:browser]\n");
    make_file("etc/stlxdm/conf.d/sheen.conf", "[taskbar:sheen]\n");
    make_file("tmp/notes.md", "notes\n");
    make_file("tmp/sheen-notes.md", "more notes\n");

    make_file("dev/notes-device", "skipped\n");
    make_file(".hidden/notes-secret.md", "hidden\n");
    mkfifo(at("tmp/notes.fifo").c_str(), 0644);

    {
        file_index index;
        auto describe = [](found_file& file) {
            file.mime_type = file.is_folder ? "inode/directory" : "text/x-test";
        };

        check(index.start(g_root, at("dev") + " " + at("proc"), describe), "the index starts");
        check_walk(index);
        check_search(index);
        check_listing(index);
        check_newest_wins(index);
    }

    std::string remove = std::string("rm -rf ") + g_root;
    if (system(remove.c_str()) != 0) {
        printf("file_index: cannot remove %s\n", g_root);
    }

    if (g_failures != 0) {
        printf("file_index: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("file_index: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
