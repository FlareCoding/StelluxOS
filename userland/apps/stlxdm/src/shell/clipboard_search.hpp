#ifndef STLXDM_SHELL_CLIPBOARD_SEARCH_HPP
#define STLXDM_SHELL_CLIPBOARD_SEARCH_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* A text the searcher reads, through a duplicate of its shared memory object that it closes once done */
class searched_text {
public:
    searched_text(uint64_t id, int object, uint64_t len) : m_id(id), m_object(object), m_len(len) {}
    ~searched_text();

    searched_text(searched_text&& other) noexcept;
    searched_text& operator=(searched_text&& other) noexcept;
    searched_text(const searched_text&) = delete;
    searched_text& operator=(const searched_text&) = delete;

    uint64_t id() const { return m_id; }
    int object() const { return m_object; }
    uint64_t len() const { return m_len; }

private:
    uint64_t m_id = 0;
    int      m_object = -1;
    uint64_t m_len = 0;
};

/* The texts of one request that hold its query */
struct text_matches {
    uint64_t              request = 0;
    std::vector<uint64_t> ids;
};

/* Searches clipboard texts past the first 64 KB the palette reads itself, on a thread of its own so a long text never
 * holds up a keystroke. Only the newest request is answered, and a newer one stops an older search early. */
class clipboard_search {
public:
    clipboard_search() = default;
    ~clipboard_search();

    clipboard_search(const clipboard_search&) = delete;
    clipboard_search& operator=(const clipboard_search&) = delete;

    /* Starts the thread. False when it or the wake pipe cannot be made. */
    bool start();

    /* Readable while an answer waits for `take()` */
    int wake_fd() const { return m_wake[0]; }

    /* Asks which of `texts` hold `query` in any ASCII case. Returns the request's number, 0 while not started. */
    uint64_t search(std::string query, std::vector<searched_text> texts);

    /* Empties the wake pipe and moves out the newest answer when one arrived since the last call */
    bool take(text_matches* matches);

private:
    struct request {
        uint64_t                   number = 0;
        std::string                query;
        std::vector<searched_text> texts;
    };

    void search_loop();
    bool holds(const searched_text& text, const std::string& query, uint64_t number) const;
    void wake();

    int m_wake[2] = { -1, -1 };

    std::mutex              m_lock;
    std::condition_variable m_request_waiting;
    bool                    m_stopping = false;
    uint64_t                m_next_request = 1;
    std::atomic<uint64_t>   m_newest_request{ 0 };
    bool                    m_has_request = false;
    request                 m_request;
    bool                    m_has_answer = false;
    text_matches            m_answer;

    std::thread m_searcher;
};

#endif
