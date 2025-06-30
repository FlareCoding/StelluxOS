#ifndef KSTL_RING_BUFFER_H
#define KSTL_RING_BUFFER_H

#include <kstl/kstl_primitive.h>
#include <core/sync.h>
#include <memory/memory.h>

namespace kstl {

/**
 * @brief Thread-safe ring buffer implementation using atomic operations
 * 
 * This ring buffer provides thread-safe single-producer/single-consumer
 * or multi-producer/multi-consumer operations depending on usage.
 * Uses atomic operations for lock-free performance in many scenarios.
 */
template<typename T>
class ring_buffer {
public:
    static_assert(kstl::is_primitive<T>::value || sizeof(T) <= 8, 
                  "ring_buffer supports primitive types or types <= 8 bytes");

private:
    static constexpr size_t DEFAULT_CAPACITY = 1024;
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    // Atomic positions and size for thread safety
    atomic<size_t> m_write_pos;
    atomic<size_t> m_read_pos;
    atomic<size_t> m_size;
    
    T* m_buffer;
    size_t m_capacity;
    size_t m_mask;  // For fast modulo operations (capacity must be power of 2)
    
    // Mutex for multi-producer/multi-consumer safety
    mutable mutex m_write_mutex = mutex();
    mutable mutex m_read_mutex = mutex();

public:
    /**
     * @brief Constructor with specified capacity
     * @param capacity Buffer capacity (will be rounded up to next power of 2)
     */
    explicit ring_buffer(size_t capacity = DEFAULT_CAPACITY) 
        : m_write_pos(0), m_read_pos(0), m_size(0) {
        
        // Round capacity up to next power of 2 for efficient modulo
        m_capacity = next_power_of_2(capacity);
        m_mask = m_capacity - 1;
        
        // Allocate buffer
        m_buffer = static_cast<T*>(aligned_alloc(sizeof(T), m_capacity * sizeof(T)));
        if (!m_buffer) {
            m_capacity = 0;
            m_mask = 0;
        }
    }

    /**
     * @brief Destructor
     */
    ~ring_buffer() {
        if (m_buffer) {
            // Call destructors for non-primitive types
            if constexpr (!kstl::is_primitive<T>::value) {
                clear();
            }
            aligned_free(m_buffer);
        }
    }

    // Non-copyable, non-movable
    ring_buffer(const ring_buffer&) = delete;
    ring_buffer& operator=(const ring_buffer&) = delete;
    ring_buffer(ring_buffer&&) = delete;
    ring_buffer& operator=(ring_buffer&&) = delete;

    /**
     * @brief Single-producer write operation (lock-free)
     * @param item Item to write
     * @return true if write successful, false if buffer full
     */
    bool push_single_producer(const T& item) {
        if (!m_buffer) return false;
        
        const size_t current_size = m_size.load();
        if (current_size >= m_capacity) {
            return false; // Buffer full
        }
        
        const size_t write_pos = m_write_pos.load();
        
        // Copy item to buffer
        if constexpr (kstl::is_primitive<T>::value) {
            m_buffer[write_pos] = item;
        } else {
            new (&m_buffer[write_pos]) T(item);
        }
        
        // Update positions atomically
        m_write_pos.store((write_pos + 1) & m_mask);
        m_size.fetch_add(1);
        
        return true;
    }

    /**
     * @brief Single-consumer read operation (lock-free)
     * @param item Reference to store the read item
     * @return true if read successful, false if buffer empty
     */
    bool pop_single_consumer(T& item) {
        if (!m_buffer) return false;
        
        const size_t current_size = m_size.load();
        if (current_size == 0) {
            return false; // Buffer empty
        }
        
        const size_t read_pos = m_read_pos.load();
        
        // Copy item from buffer
        if constexpr (kstl::is_primitive<T>::value) {
            item = m_buffer[read_pos];
        } else {
            item = m_buffer[read_pos];
            m_buffer[read_pos].~T();
        }
        
        // Update positions atomically
        m_read_pos.store((read_pos + 1) & m_mask);
        m_size.fetch_sub(1);
        
        return true;
    }

    /**
     * @brief Multi-producer write operation (uses mutex)
     * @param item Item to write
     * @return true if write successful, false if buffer full
     */
    bool push(const T& item) {
        mutex_guard guard(m_write_mutex);
        return push_single_producer(item);
    }

    /**
     * @brief Multi-consumer read operation (uses mutex)
     * @param item Reference to store the read item
     * @return true if read successful, false if buffer empty
     */
    bool pop(T& item) {
        mutex_guard guard(m_read_mutex);
        return pop_single_consumer(item);
    }

    /**
     * @brief Bulk write operation
     * @param items Pointer to array of items
     * @param count Number of items to write
     * @return Number of items actually written
     */
    size_t write_bulk(const T* items, size_t count) {
        if (!m_buffer || !items || count == 0) return 0;
        
        mutex_guard guard(m_write_mutex);
        
        size_t written = 0;
        for (size_t i = 0; i < count && written < count; ++i) {
            if (push_single_producer(items[i])) {
                ++written;
            } else {
                break; // Buffer full
            }
        }
        
        return written;
    }

    /**
     * @brief Bulk read operation
     * @param items Pointer to array to store items
     * @param count Maximum number of items to read
     * @return Number of items actually read
     */
    size_t read_bulk(T* items, size_t count) {
        if (!m_buffer || !items || count == 0) return 0;
        
        mutex_guard guard(m_read_mutex);
        
        size_t read = 0;
        for (size_t i = 0; i < count && read < count; ++i) {
            if (pop_single_consumer(items[i])) {
                ++read;
            } else {
                break; // Buffer empty
            }
        }
        
        return read;
    }

    /**
     * @brief Check current size of the buffer
     * @return Number of items currently in buffer
     */
    size_t size() const {
        return m_size.load();
    }

    /**
     * @brief Check if buffer is empty
     * @return true if empty, false otherwise
     */
    bool empty() const {
        return size() == 0;
    }

    /**
     * @brief Check if buffer is full
     * @return true if full, false otherwise
     */
    bool full() const {
        return size() >= m_capacity;
    }

    /**
     * @brief Get buffer capacity
     * @return Maximum number of items the buffer can hold
     */
    size_t capacity() const {
        return m_capacity;
    }

    /**
     * @brief Get available space in buffer
     * @return Number of items that can be written without blocking
     */
    size_t available_space() const {
        return m_capacity - size();
    }

    /**
     * @brief Clear all items from the buffer
     */
    void clear() {
        mutex_guard write_guard(m_write_mutex);
        mutex_guard read_guard(m_read_mutex);
        
        if constexpr (!kstl::is_primitive<T>::value) {
            // Call destructors for non-primitive types
            T dummy;
            while (pop_single_consumer(dummy)) {
                // Items are destroyed in pop_single_consumer
            }
        }
        
        m_write_pos.store(0);
        m_read_pos.store(0);
        m_size.store(0);
    }

    /**
     * @brief Reset buffer to initial state
     */
    void reset() {
        clear();
    }

private:
    /**
     * @brief Calculate next power of 2 greater than or equal to n
     */
    static size_t next_power_of_2(size_t n) {
        if (n == 0) return 1;
        
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return ++n;
    }
    
    /**
     * @brief Aligned memory allocation
     */
    static void* aligned_alloc(size_t alignment, size_t size) {
        // For kernel use, just use regular allocation since new/delete
        // should provide sufficient alignment for most cases
        (void)alignment; // Suppress unused parameter warning
        return new uint8_t[size];
    }
    
    /**
     * @brief Aligned memory deallocation
     */
    static void aligned_free(void* ptr) {
        if (ptr) {
            delete[] static_cast<uint8_t*>(ptr);
        }
    }
};

/**
 * @brief Specialized ring buffer for byte data (common use case)
 */
using byte_ring_buffer = ring_buffer<uint8_t>;

} // namespace kstl

#endif // KSTL_RING_BUFFER_H
