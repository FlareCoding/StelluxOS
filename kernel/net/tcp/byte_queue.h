#ifndef STELLUX_NET_TCP_BYTE_QUEUE_H
#define STELLUX_NET_TCP_BYTE_QUEUE_H

#include "common/types.h"
#include "common/list.h"

namespace net {
namespace tcp {

constexpr size_t CHUNK_SIZE    = 2048; // the heap slab packets use
constexpr size_t MIN_BUF       = 16 * 1024;
constexpr size_t MAX_BUF       = 1024 * 1024;
constexpr size_t GLOBAL_BUDGET = 16 * 1024 * 1024;

struct chunk {
    list::node link;
    uint8_t    data[CHUNK_SIZE - sizeof(list::node)];
};
static_assert(sizeof(chunk) == CHUNK_SIZE);

constexpr size_t CHUNK_PAYLOAD = sizeof(chunk::data);

/**
 * A FIFO of bytes in CHUNK_SIZE chunks, the send queue and the receive queue
 * of a connection. Bytes are appended at the tail, copied out at any offset
 * from the head, and consumed from the head. Chunks come one at a time from
 * the global budget as the tail fills and go back as the head empties, so
 * growing or shrinking never copies. The owner serializes every call.
 */
class byte_queue {
public:
    void init(size_t limit_chunks);

    size_t size() const { return m_size; }
    size_t chunk_count() const { return m_chunks.size(); }
    size_t limit() const { return m_limit; }
    void set_limit(size_t chunks) { m_limit = chunks; }

    /**
     * @brief Bytes `append` can take before the chunk limit stops it.
     */
    size_t free_space() const;

    /**
     * @brief Appends up to `len` bytes from `src`.
     * @return The bytes appended, fewer than `len` when the chunk limit or the
     *         global budget stops it.
     */
    size_t append(const void* src, size_t len);

    /**
     * @brief Copies up to `len` bytes starting `offset` bytes past the head
     * into `dst`, leaving them in the queue.
     * @return The bytes copied, zero when `offset` is past the end.
     */
    size_t copy_out(size_t offset, void* dst, size_t len) const;

    /**
     * @brief Removes up to `len` bytes from the head and returns the chunks
     * this empties to the budget.
     * @return The bytes removed.
     */
    size_t consume(size_t len);

    /**
     * @brief Removes everything and returns every chunk to the budget.
     */
    void clear();

private:
    size_t tail_room() const;
    bool grow();
    void release_front();

    list::head<chunk, &chunk::link> m_chunks;
    size_t                          m_head_offset;
    size_t                          m_size;
    size_t                          m_limit;
};

/**
 * @brief Takes `bytes` from the budget every queue and record shares.
 * @return false, taking nothing, when the budget would be exceeded.
 */
bool reserve_budget(size_t bytes);

/**
 * @brief Gives `bytes` back to the budget.
 */
void release_budget(size_t bytes);

/**
 * @brief Bytes of the budget in use across the stack.
 */
size_t budget_in_use();

void __dbg_test_set_budget(size_t bytes);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_BYTE_QUEUE_H
