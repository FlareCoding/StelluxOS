#include "file_index.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

/* An index stops growing past this many entries, which bounds its memory on a tree of millions */
constexpr size_t INDEX_MAX = 200000;

/* How often a walk tells the files' head its count */
constexpr uint64_t PROGRESS_INTERVAL_NS = 100000000ull;

/* An answered folder previews this many of its entries */
constexpr size_t PREVIEW_NAMES = 3;

static uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

static char folded(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

static bool is_before_folded(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return folded(x) < folded(y);
    });
}

static bool starts_with_folded(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }

    for (size_t i = 0; i < prefix.size(); i++) {
        if (folded(text[i]) != folded(prefix[i])) {
            return false;
        }
    }

    return true;
}

static std::string joined_path(const std::string& folder, const char* name) {
    return folder.back() == '/' ? folder + name : folder + "/" + name;
}

/* The entries of `folder` other than . and .., or none when it cannot be read */
static std::vector<std::string> entry_names(const std::string& folder) {
    std::vector<std::string> names;
    DIR* dir = opendir(folder.c_str());
    if (!dir) {
        return names;
    }

    while (dirent* entry = readdir(dir)) {
        bool is_self_or_parent = strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0;
        if (!is_self_or_parent) {
            names.emplace_back(entry->d_name);
        }
    }

    closedir(dir);
    return names;
}

/* A folder's size is its count of entries, and its preview the first few by name */
static void describe_folder(found_file& folder) {
    std::vector<std::string> names = entry_names(folder.path);
    std::sort(names.begin(), names.end());
    folder.size = names.size();

    for (size_t i = 0; i < names.size() && i < PREVIEW_NAMES; i++) {
        folder.preview += folder.preview.empty() ? names[i] : ", " + names[i];
    }
}

static found_file make_found(const std::string& path, const struct stat& st) {
    found_file file;
    file.path = path;
    file.name_start = static_cast<uint32_t>(path.rfind('/') + 1);
    file.is_folder = S_ISDIR(st.st_mode);
    file.size = file.is_folder ? 0 : static_cast<uint64_t>(st.st_size);

    return file;
}

file_index::~file_index() {
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stopping = true;
    }

    m_walk_wanted.notify_all();
    m_request_waiting.notify_all();
    if (m_walker.joinable()) {
        m_walker.join();
    }

    if (m_searcher.joinable()) {
        m_searcher.join();
    }

    for (int fd : m_wake) {
        if (fd >= 0) {
            close(fd);
        }
    }
}

bool file_index::start(const std::string& root, const std::string& skip, file_describer describe) {
    m_root = root;
    m_describe = std::move(describe);
    size_t at = 0;
    while (at < skip.size()) {
        size_t end = skip.find(' ', at);
        end = end == std::string::npos ? skip.size() : end;
        if (end > at) {
            m_skip.push_back(skip.substr(at, end - at));
        }

        at = end + 1;
    }

    if (pipe(m_wake) != 0) {
        m_wake[0] = m_wake[1] = -1;
        return false;
    }

    fcntl(m_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wake[1], F_SETFL, O_NONBLOCK);

    try {
        m_walker = std::thread(&file_index::walk_loop, this);
        m_searcher = std::thread(&file_index::search_loop, this);
    } catch (const std::system_error&) {
        return false;
    }

    return true;
}

void file_index::rewalk() {
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (m_walk.walking) {
            return;
        }

        m_should_walk = true;
    }

    m_walk_wanted.notify_one();
}

uint64_t file_index::search(std::string query, size_t limit) {
    uint64_t number;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        number = m_next_request++;
        m_request = { number, request_kind::search, std::move(query), {}, limit };
        m_has_request = true;
    }

    m_request_waiting.notify_one();
    return number;
}

uint64_t file_index::list(std::string folder, std::string prefix) {
    uint64_t number;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        number = m_next_request++;
        m_request = { number, request_kind::list, std::move(folder), std::move(prefix), 0 };
        m_has_request = true;
    }

    m_request_waiting.notify_one();
    return number;
}

bool file_index::take(file_answer* answer, walk_state* walk) {
    char drained[64];
    while (read(m_wake[0], drained, sizeof(drained)) > 0) {
    }

    std::lock_guard<std::mutex> guard(m_lock);
    *walk = m_walk;
    if (!m_has_answer) {
        return false;
    }

    *answer = std::move(m_answer);
    m_has_answer = false;

    return true;
}

/* One byte is enough to wake the compositor, and a full pipe already will */
void file_index::wake() {
    char byte = 1;
    ssize_t written = write(m_wake[1], &byte, 1);
    (void)written;
}

void file_index::walk_loop() {
    std::unique_lock<std::mutex> lock(m_lock);
    for (;;) {
        m_walk_wanted.wait(lock, [this] { return m_stopping || m_should_walk; });
        if (m_stopping) {
            return;
        }

        m_should_walk = false;
        m_walk = { true, 0 };
        lock.unlock();
        wake();

        auto index = std::make_shared<std::vector<found_file>>();
        walk(*index);

        lock.lock();
        m_walk = { false, index->size() };
        m_index = std::move(index);
        lock.unlock();
        wake();
        lock.lock();
    }
}

/* Depth first from the root, entering no skipped folder, no hidden entry and no symbolic link, and keeping files and
 * folders but no socket, pipe or device */
void file_index::walk(std::vector<found_file>& index) {
    std::vector<std::string> folders = { m_root };
    while (!folders.empty() && index.size() < INDEX_MAX) {
        std::string folder = std::move(folders.back());
        folders.pop_back();

        for (const std::string& name : entry_names(folder)) {
            std::string path = joined_path(folder, name.c_str());
            struct stat st;
            bool is_skipped = std::find(m_skip.begin(), m_skip.end(), path) != m_skip.end();
            if (name[0] == '.' || is_skipped || lstat(path.c_str(), &st) != 0) {
                continue;
            }

            if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
                continue;
            }

            index.push_back(make_found(path, st));
            if (S_ISDIR(st.st_mode)) {
                folders.push_back(path);
            }
        }

        std::lock_guard<std::mutex> guard(m_lock);
        if (m_stopping) {
            return;
        }

        m_walk.found = index.size();
        uint64_t now = monotonic_ns();
        if (now - m_last_wake_ns >= PROGRESS_INTERVAL_NS) {
            m_last_wake_ns = now;
            wake();
        }
    }
}

void file_index::search_loop() {
    std::unique_lock<std::mutex> lock(m_lock);
    for (;;) {
        m_request_waiting.wait(lock, [this] { return m_stopping || m_has_request; });
        if (m_stopping) {
            return;
        }

        request asked = std::move(m_request);
        m_has_request = false;
        lock.unlock();

        file_answer answer = asked.kind == request_kind::search ? answer_search(asked) : answer_listing(asked);

        lock.lock();
        m_answer = std::move(answer);
        m_has_answer = true;
        lock.unlock();
        wake();
        lock.lock();
    }
}

/* Better matches first, then shorter names, then paths in order, so a file near the root wins a tie */
file_answer file_index::answer_search(const request& asked) {
    std::shared_ptr<const std::vector<found_file>> index;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        index = m_index;
    }

    struct ranked {
        const found_file* file;
        query_match       match;
    };

    std::vector<ranked> found;
    for (const found_file& file : *index) {
        query_match match = match_query(asked.text, file.name(), "");
        if (match.quality != match_quality::none) {
            found.push_back({ &file, std::move(match) });
        }
    }

    size_t shown = std::min(found.size(), asked.limit);
    std::partial_sort(found.begin(), found.begin() + static_cast<long>(shown), found.end(),
                      [](const ranked& a, const ranked& b) {
        if (a.match.quality != b.match.quality) {
            return a.match.quality < b.match.quality;
        }

        if (a.file->name().size() != b.file->name().size()) {
            return a.file->name().size() < b.file->name().size();
        }

        return a.file->path < b.file->path;
    });

    file_answer answer;
    answer.request = asked.number;
    answer.total = found.size();
    for (size_t i = 0; i < shown; i++) {
        answer.files.push_back(*found[i].file);
        answer.matches.push_back(std::move(found[i].match));
        describe(answer.files.back());
    }

    return answer;
}

void file_index::describe(found_file& file) const {
    if (file.is_folder) {
        describe_folder(file);
    }

    if (m_describe) {
        m_describe(file);
    }
}

/* Folders first, by name in any case. Hidden entries show once the prefix itself starts with a dot. */
file_answer file_index::answer_listing(const request& asked) {
    file_answer answer;
    answer.request = asked.number;

    bool shows_hidden = !asked.prefix.empty() && asked.prefix[0] == '.';
    for (const std::string& name : entry_names(asked.text)) {
        std::string path = joined_path(asked.text, name.c_str());
        struct stat st;
        bool is_shown = (shows_hidden || name[0] != '.') && starts_with_folded(name, asked.prefix);
        if (!is_shown || lstat(path.c_str(), &st) != 0) {
            continue;
        }

        answer.files.push_back(make_found(path, st));
    }

    std::sort(answer.files.begin(), answer.files.end(), [](const found_file& a, const found_file& b) {
        if (a.is_folder != b.is_folder) {
            return a.is_folder;
        }

        return is_before_folded(a.name(), b.name());
    });

    uint32_t marked = static_cast<uint32_t>(asked.prefix.size());
    for (found_file& file : answer.files) {
        describe(file);
        answer.matches.push_back(marked > 0 ? query_match{ match_quality::prefix, { { 0, marked } } } : query_match{});
    }

    answer.total = answer.files.size();
    return answer;
}
