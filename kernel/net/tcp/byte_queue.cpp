#include "net/tcp/byte_queue.h"
#include "sync/atomic.h"
#include "mm/heap.h"
#include "common/string.h"

namespace net {
namespace tcp {

static sync::atomic<size_t> g_budget_in_use;
static size_t               g_budget_limit = GLOBAL_BUDGET;

static const chunk* next_chunk(const chunk* current) {
    return list::node_to_entry<chunk, &chunk::link>(current->link.next);
}

bool reserve_budget(size_t bytes) {
    size_t in_use = g_budget_in_use.fetch_add_relaxed(bytes) + bytes;
    if (in_use > g_budget_limit) {
        g_budget_in_use.fetch_sub_relaxed(bytes);
        return false;
    }

    return true;
}

void release_budget(size_t bytes) {
    g_budget_in_use.fetch_sub_relaxed(bytes);
}

size_t budget_in_use() {
    return g_budget_in_use.load_relaxed();
}

void __dbg_test_set_budget(size_t bytes) {
    g_budget_limit = bytes;
}

void byte_queue::init(size_t limit_chunks) {
    m_chunks.init();
    m_head_offset = 0;
    m_size = 0;
    m_limit = limit_chunks;
}

size_t byte_queue::tail_room() const {
    if (m_chunks.empty()) {
        return 0;
    }

    return m_chunks.size() * CHUNK_PAYLOAD - m_head_offset - m_size;
}

size_t byte_queue::free_space() const {
    size_t more_chunks = m_chunks.size() < m_limit ? m_limit - m_chunks.size() : 0;
    return tail_room() + more_chunks * CHUNK_PAYLOAD;
}

bool byte_queue::grow() {
    if (m_chunks.size() >= m_limit || !reserve_budget(CHUNK_SIZE)) {
        return false;
    }

    chunk* fresh = static_cast<chunk*>(heap::ualloc(sizeof(chunk)));
    if (!fresh) {
        release_budget(CHUNK_SIZE);
        return false;
    }

    fresh->link = {};
    m_chunks.push_back(fresh);

    return true;
}

void byte_queue::release_front() {
    chunk* front = m_chunks.pop_front();
    heap::ufree(front);

    release_budget(CHUNK_SIZE);
}

size_t byte_queue::append(const void* src, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(src);
    size_t appended = 0;

    while (appended < len) {
        size_t room = tail_room();
        if (room == 0) {
            if (!grow()) {
                break;
            }

            room = CHUNK_PAYLOAD;
        }

        size_t fill = CHUNK_PAYLOAD - room;
        size_t count = len - appended < room ? len - appended : room;
        string::memcpy(m_chunks.back()->data + fill, bytes + appended, count);
        m_size += count;
        appended += count;
    }

    return appended;
}

size_t byte_queue::copy_out(size_t offset, void* dst, size_t len) const {
    if (offset >= m_size) {
        return 0;
    }

    size_t remaining = m_size - offset < len ? m_size - offset : len;
    size_t position = m_head_offset + offset;
    const chunk* current = m_chunks.front();
    while (position >= CHUNK_PAYLOAD) {
        current = next_chunk(current);
        position -= CHUNK_PAYLOAD;
    }

    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t copied = 0;
    while (copied < remaining) {
        size_t available = CHUNK_PAYLOAD - position;
        size_t count = remaining - copied < available ? remaining - copied : available;
        string::memcpy(out + copied, current->data + position, count);

        copied += count;
        position = 0;
        current = next_chunk(current);
    }

    return copied;
}

size_t byte_queue::consume(size_t len) {
    size_t removed = len < m_size ? len : m_size;
    m_size -= removed;
    m_head_offset += removed;

    while (m_head_offset >= CHUNK_PAYLOAD) {
        release_front();
        m_head_offset -= CHUNK_PAYLOAD;
    }

    if (m_size == 0) {
        clear();
    }

    return removed;
}

void byte_queue::clear() {
    while (!m_chunks.empty()) {
        release_front();
    }

    m_head_offset = 0;
    m_size = 0;
}

} // namespace tcp
} // namespace net
