#include <net/unix_socket_buffer.h>
#include <memory/memory.h>
#include <core/klog.h>

namespace net {

unix_socket_buffer::unix_socket_buffer(size_t capacity) 
    : m_capacity(capacity), m_head(0), m_tail(0), m_size(0) {
    
    if (capacity == 0) {
        kprint("[UNIX_SOCKET] Error: Buffer capacity cannot be zero\n");
        m_capacity = DEFAULT_BUFFER_SIZE;
    }
    
    m_buffer = new uint8_t[m_capacity];
    if (!m_buffer) {
        kprint("[UNIX_SOCKET] Error: Failed to allocate buffer of size %llu\n", m_capacity);
        // This is a critical error - we can't function without a buffer
        m_capacity = 0;
        return;
    }
    
    // Initialize buffer to zero for debugging
    memset(m_buffer, 0, m_capacity);
}

unix_socket_buffer::~unix_socket_buffer() {
    if (m_buffer) {
        delete[] m_buffer;
        m_buffer = nullptr;
    }
}

size_t unix_socket_buffer::write(const void* data, size_t size) {
    if (!data || size == 0 || !m_buffer) {
        return 0;
    }
    
    mutex_guard guard(m_lock);
    
    size_t current_size = m_size.load();
    size_t available_space = m_capacity - current_size;
    
    if (available_space == 0) {
        return 0; // Buffer is full
    }
    
    // Limit write size to available space
    size_t bytes_to_write = (size > available_space) ? available_space : size;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t bytes_written = 0;
    
    // Handle circular buffer wrapping - may need to write in two parts
    while (bytes_written < bytes_to_write) {
        size_t contiguous_space = _contiguous_write_space();
        if (contiguous_space == 0) {
            break; // Should not happen, but safety check
        }
        
        size_t chunk_size = bytes_to_write - bytes_written;
        if (chunk_size > contiguous_space) {
            chunk_size = contiguous_space;
        }
        
        // Copy data to buffer
        memcpy(m_buffer + m_head, src + bytes_written, chunk_size);
        
        // Update head position
        m_head = (m_head + chunk_size) % m_capacity;
        bytes_written += chunk_size;
    }
    
    // Update size atomically
    m_size.fetch_add(bytes_written);
    
    return bytes_written;
}

size_t unix_socket_buffer::read(void* buffer, size_t size) {
    if (!buffer || size == 0 || !m_buffer) {
        return 0;
    }
    
    mutex_guard guard(m_lock);
    
    size_t current_size = m_size.load();
    if (current_size == 0) {
        return 0; // Buffer is empty
    }
    
    // Limit read size to available data
    size_t bytes_to_read = (size > current_size) ? current_size : size;
    uint8_t* dest = static_cast<uint8_t*>(buffer);
    size_t bytes_read = 0;
    
    // Handle circular buffer wrapping - may need to read in two parts
    while (bytes_read < bytes_to_read) {
        size_t contiguous_data = _contiguous_read_space();
        if (contiguous_data == 0) {
            break; // Should not happen, but safety check
        }
        
        size_t chunk_size = bytes_to_read - bytes_read;
        if (chunk_size > contiguous_data) {
            chunk_size = contiguous_data;
        }
        
        // Copy data from buffer
        memcpy(dest + bytes_read, m_buffer + m_tail, chunk_size);
        
        // Update tail position
        m_tail = (m_tail + chunk_size) % m_capacity;
        bytes_read += chunk_size;
    }
    
    // Update size atomically
    m_size.fetch_sub(bytes_read);
    
    return bytes_read;
}

bool unix_socket_buffer::has_data() const {
    return m_size.load() > 0;
}

bool unix_socket_buffer::has_space() const {
    return m_size.load() < m_capacity;
}

size_t unix_socket_buffer::available_bytes() const {
    return m_size.load();
}

size_t unix_socket_buffer::free_space() const {
    return m_capacity - m_size.load();
}

void unix_socket_buffer::clear() {
    mutex_guard guard(m_lock);
    
    m_head = 0;
    m_tail = 0;
    m_size.store(0);
    
    // Optional: Clear buffer memory for security
    if (m_buffer) {
        memset(m_buffer, 0, m_capacity);
    }
}

size_t unix_socket_buffer::_next_index(size_t index) const {
    return (index + 1) % m_capacity;
}

size_t unix_socket_buffer::_contiguous_write_space() const {
    size_t current_size = m_size.load();
    size_t available_space = m_capacity - current_size;
    
    if (available_space == 0) {
        return 0;
    }
    
    // If head is before tail, we can write until we reach tail
    // If head is at or after tail, we can write until end of buffer
    if (m_head < m_tail) {
        return m_tail - m_head;
    } else {
        // Write to end of buffer, unless we would wrap to tail
        size_t space_to_end = m_capacity - m_head;
        if (m_tail == 0 && current_size > 0) {
            // Special case: can't write to position 0 if tail is there
            return space_to_end - 1;
        }
        return space_to_end;
    }
}

size_t unix_socket_buffer::_contiguous_read_space() const {
    size_t current_size = m_size.load();
    
    if (current_size == 0) {
        return 0;
    }
    
    // If tail is before head, we can read until we reach head
    // If tail is at or after head, we can read until end of buffer
    if (m_tail < m_head) {
        return m_head - m_tail;
    } else {
        // Read to end of buffer
        return m_capacity - m_tail;
    }
}

} // namespace net
