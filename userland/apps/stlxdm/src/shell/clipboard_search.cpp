#include "clipboard_search.hpp"

#include "../clipboard.hpp"

#include <fcntl.h>
#include <string_view>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>

/* A search looks for a newer request between chunks of this many bytes */
constexpr size_t CHUNK_BYTES = 1024 * 1024;

searched_text::~searched_text() {
    if (m_object >= 0) {
        close(m_object);
    }
}

searched_text::searched_text(searched_text&& other) noexcept
    : m_id(other.m_id), m_object(other.m_object), m_len(other.m_len) {
    other.m_object = -1;
}

searched_text& searched_text::operator=(searched_text&& other) noexcept {
    if (this != &other) {
        if (m_object >= 0) {
            close(m_object);
        }

        m_id = other.m_id;
        m_object = other.m_object;
        m_len = other.m_len;
        other.m_object = -1;
    }

    return *this;
}

clipboard_search::~clipboard_search() {
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stopping = true;
        m_newest_request = 0;
    }

    m_request_waiting.notify_all();
    if (m_searcher.joinable()) {
        m_searcher.join();
    }

    for (int fd : m_wake) {
        if (fd >= 0) {
            close(fd);
        }
    }
}

bool clipboard_search::start() {
    if (pipe(m_wake) != 0) {
        m_wake[0] = m_wake[1] = -1;
        return false;
    }

    fcntl(m_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wake[1], F_SETFL, O_NONBLOCK);

    try {
        m_searcher = std::thread(&clipboard_search::search_loop, this);
    } catch (const std::system_error&) {
        return false;
    }

    return true;
}

/* A request the searcher never took closes its texts as this one replaces it */
uint64_t clipboard_search::search(std::string query, std::vector<searched_text> texts) {
    if (!m_searcher.joinable()) {
        return 0;
    }

    uint64_t number;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        number = m_next_request++;
        m_newest_request = number;
        m_request = { number, std::move(query), std::move(texts) };
        m_has_request = true;
    }

    m_request_waiting.notify_one();
    return number;
}

bool clipboard_search::take(text_matches* matches) {
    char drained[64];
    while (read(m_wake[0], drained, sizeof(drained)) > 0) {
    }

    std::lock_guard<std::mutex> guard(m_lock);
    if (!m_has_answer) {
        return false;
    }

    *matches = std::move(m_answer);
    m_has_answer = false;

    return true;
}

/* One byte is enough to wake the compositor, and a full pipe already will */
void clipboard_search::wake() {
    char byte = 1;
    ssize_t written = write(m_wake[1], &byte, 1);
    (void)written;
}

/* A search a newer request overtook is dropped unanswered */
void clipboard_search::search_loop() {
    std::unique_lock<std::mutex> lock(m_lock);
    for (;;) {
        m_request_waiting.wait(lock, [this] { return m_stopping || m_has_request; });
        if (m_stopping) {
            return;
        }

        request asked = std::move(m_request);
        m_has_request = false;
        lock.unlock();

        text_matches found = { asked.number, {} };
        for (const searched_text& text : asked.texts) {
            if (holds(text, asked.query, asked.number)) {
                found.ids.push_back(text.id());
            }
        }

        asked.texts.clear();
        lock.lock();
        if (asked.number == m_newest_request) {
            m_answer = std::move(found);
            m_has_answer = true;
            wake();
        }
    }
}

/* A match may straddle two chunks, so each reads on past its end by the query's length less one */
bool clipboard_search::holds(const searched_text& text, const std::string& query, uint64_t number) const {
    if (text.len() == 0 || query.empty()) {
        return false;
    }

    void* shared = mmap(nullptr, text.len(), PROT_READ, MAP_SHARED, text.object(), 0);
    if (shared == MAP_FAILED) {
        return false;
    }

    std::string_view bytes(static_cast<const char*>(shared), text.len());
    bool is_found = false;
    for (size_t at = 0; at < bytes.size() && !is_found && m_newest_request == number; at += CHUNK_BYTES) {
        std::string_view chunk = bytes.substr(at, CHUNK_BYTES + query.size() - 1);
        is_found = find_folded(chunk, query) != std::string_view::npos;
    }

    munmap(shared, text.len());
    return is_found;
}
