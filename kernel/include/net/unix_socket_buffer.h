#ifndef UNIX_SOCKET_BUFFER_H
#define UNIX_SOCKET_BUFFER_H

#include <core/sync.h>
#include <core/types.h>
#include <memory/memory.h>

namespace net {

/**
 * @class unix_socket_buffer
 * @brief Thread-safe circular buffer optimized for Unix stream socket data transfer.
 * 
 * This buffer provides efficient, thread-safe data transfer between socket endpoints.
 * It uses a fixed-size circular buffer with atomic operations for size tracking
 * and mutex protection for buffer operations.
 */
class unix_socket_buffer {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 8192; // 8KB default

    /**
     * @brief Constructs a socket buffer with specified capacity.
     * @param capacity Buffer size in bytes (must be > 0)
     */
    explicit unix_socket_buffer(size_t capacity = DEFAULT_BUFFER_SIZE);

    /**
     * @brief Destructor - frees allocated buffer memory.
     */
    ~unix_socket_buffer();

    // Non-copyable, non-movable for thread safety
    unix_socket_buffer(const unix_socket_buffer&) = delete;
    unix_socket_buffer& operator=(const unix_socket_buffer&) = delete;
    unix_socket_buffer(unix_socket_buffer&&) = delete;
    unix_socket_buffer& operator=(unix_socket_buffer&&) = delete;

    /**
     * @brief Writes data to the buffer (non-blocking).
     * 
     * Attempts to write as much data as possible to the buffer without blocking.
     * If the buffer is full, only partial data may be written.
     * 
     * @param data Pointer to data to write
     * @param size Number of bytes to write
     * @return Number of bytes actually written (0 if buffer is full)
     */
    size_t write(const void* data, size_t size);

    /**
     * @brief Reads data from the buffer (non-blocking).
     * 
     * Attempts to read as much data as available from the buffer without blocking.
     * If the buffer is empty, returns 0.
     * 
     * @param buffer Buffer to read data into
     * @param size Maximum number of bytes to read
     * @return Number of bytes actually read (0 if buffer is empty)
     */
    size_t read(void* buffer, size_t size);

    /**
     * @brief Checks if data is available for reading.
     * @return True if at least one byte is available for reading
     */
    bool has_data() const;

    /**
     * @brief Checks if buffer has space for writing.
     * @return True if at least one byte of space is available
     */
    bool has_space() const;

    /**
     * @brief Gets the number of bytes available for reading.
     * @return Number of bytes that can be read
     */
    size_t available_bytes() const;

    /**
     * @brief Gets the number of bytes available for writing.
     * @return Number of bytes that can be written
     */
    size_t free_space() const;

    /**
     * @brief Gets the total capacity of the buffer.
     * @return Buffer capacity in bytes
     */
    size_t capacity() const { return m_capacity; }

    /**
     * @brief Clears all data from the buffer.
     * 
     * Resets the buffer to empty state. This operation is thread-safe.
     */
    void clear();

private:
    uint8_t* m_buffer;              // Circular buffer memory
    size_t m_capacity;              // Total buffer capacity
    size_t m_head;                  // Write position (producer index)
    size_t m_tail;                  // Read position (consumer index)
    atomic<size_t> m_size;          // Current number of bytes in buffer
    mutable mutex m_lock = mutex(); // Protects buffer operations

    /**
     * @brief Calculates the next index in the circular buffer.
     * @param index Current index
     * @return Next index, wrapping around if necessary
     */
    size_t _next_index(size_t index) const;

    /**
     * @brief Calculates available contiguous write space from current head.
     * @return Number of contiguous bytes that can be written
     */
    size_t _contiguous_write_space() const;

    /**
     * @brief Calculates available contiguous read space from current tail.
     * @return Number of contiguous bytes that can be read
     */
    size_t _contiguous_read_space() const;
};

} // namespace net

#endif // UNIX_SOCKET_BUFFER_H

