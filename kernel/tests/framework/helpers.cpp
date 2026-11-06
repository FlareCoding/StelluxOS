#include "helpers.h"
#include "common/string.h"
#include "mm/heap.h"

namespace test_helpers {

uint8_t* shared_zeroed_buffer::take(size_t bytes) {
    if (m_buffer == nullptr) {
        m_buffer = static_cast<uint8_t*>(heap::uzalloc(bytes));
        m_bytes = bytes;
        m_is_taken = m_buffer != nullptr;
        return m_buffer;
    }

    if (m_is_taken || bytes != m_bytes) {
        return static_cast<uint8_t*>(heap::uzalloc(bytes));
    }

    clear();
    m_is_taken = true;

    return m_buffer;
}

void shared_zeroed_buffer::give_back(uint8_t* buffer) {
    if (buffer == m_buffer) {
        m_is_taken = false;
    } else {
        heap::ufree(buffer);
    }
}

void shared_zeroed_buffer::free() {
    heap::ufree(m_buffer);
    m_buffer = nullptr;
}

void shared_zeroed_buffer::clear() {
    uint64_t* words = reinterpret_cast<uint64_t*>(m_buffer);
    size_t word_count = m_bytes / sizeof(uint64_t);

    // A word at a time, an eighth of the stores `string::memset` makes byte by byte
    for (size_t i = 0; i < word_count; i++) {
        words[i] = 0;
    }

    string::memset(m_buffer + word_count * sizeof(uint64_t), 0, m_bytes % sizeof(uint64_t));
}

} // namespace test_helpers
