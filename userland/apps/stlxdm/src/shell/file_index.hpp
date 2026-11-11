#ifndef STLXDM_SHELL_FILE_INDEX_HPP
#define STLXDM_SHELL_FILE_INDEX_HPP

#include "quick_match.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/* One file or folder the walk found or a listing holds */
struct found_file {
    std::string path;
    uint32_t    name_start = 0; /* where the name starts in `path` */
    uint64_t    size = 0;       /* bytes of a file, entries of an answered folder */
    bool        is_folder = false;
    std::string preview;        /* an answered folder's first entries, comma separated */
    const char* mime_type = nullptr;   /* with `description`, what the describer says an answered file is */
    const char* description = nullptr;

    std::string_view name() const { return std::string_view(path).substr(name_start); }
};

/* Says what an answered file is, run on the searcher for each file an answer holds */
using file_describer = std::function<void(found_file& file)>;

/* The answer to one search or listing, for the request it names */
struct file_answer {
    uint64_t                 request = 0;
    std::vector<found_file>  files;     /* best first, at most the request's limit */
    std::vector<query_match> matches;   /* one per file, the runs of its name the query matched */
    size_t                   total = 0; /* every match, those past the limit included */
};

/* What the files' head shows of the walk */
struct walk_state {
    bool   walking = false;
    size_t found = 0; /* files and folders indexed so far, or by the last walk */
};

/* Files for Quick Actions, found on two threads so the compositor's never walks or scores them. The walker builds
 * a fresh index on each `rewalk()` while the last one answers, and the searcher answers only the newest request. */
class file_index {
public:
    file_index() = default;
    ~file_index();

    file_index(const file_index&) = delete;
    file_index& operator=(const file_index&) = delete;

    /* Starts both threads over `root`, skipping hidden entries and the space separated folders in `skip`, with
     * `describe` filling in each answered file. False when a thread or the wake pipe cannot be made. */
    bool start(const std::string& root, const std::string& skip, file_describer describe = nullptr);

    /* Readable while an answer or a change of the walk waits for `take()` */
    int wake_fd() const { return m_wake[0]; }

    /* Walks the tree again unless a walk is under way */
    void rewalk();

    /* Asks for the indexed files whose names `query` matches best, at most `limit`. Returns the request's number. */
    uint64_t search(std::string query, size_t limit);

    /* Asks for the entries of `folder` whose names start with `prefix` in any case, folders first. Returns the
     * request's number. */
    uint64_t list(std::string folder, std::string prefix);

    /* Empties the wake pipe, writes the walk's state, and moves out the newest answer when one arrived since the
     * last call */
    bool take(file_answer* answer, walk_state* walk);

private:
    enum class request_kind { search, list };

    struct request {
        uint64_t     number = 0;
        request_kind kind = request_kind::search;
        std::string  text;   /* the query, or the folder listed */
        std::string  prefix; /* a listing's filter */
        size_t       limit = 0;
    };

    void walk_loop();
    void search_loop();
    void walk(std::vector<found_file>& index);
    file_answer answer_search(const request& asked);
    file_answer answer_listing(const request& asked);
    void describe(found_file& file) const;
    void wake();

    std::string              m_root;
    std::vector<std::string> m_skip;
    file_describer           m_describe;
    int                      m_wake[2] = { -1, -1 };

    std::mutex              m_lock;
    std::condition_variable m_walk_wanted;
    std::condition_variable m_request_waiting;
    bool                    m_stopping = false;
    bool                    m_should_walk = false;
    walk_state              m_walk;
    uint64_t                m_last_wake_ns = 0;

    std::shared_ptr<const std::vector<found_file>> m_index = std::make_shared<std::vector<found_file>>();
    uint64_t                                       m_next_request = 1;
    bool                                           m_has_request = false;
    request                                        m_request;
    bool                                           m_has_answer = false;
    file_answer                                    m_answer;

    std::thread m_walker;
    std::thread m_searcher;
};

#endif
