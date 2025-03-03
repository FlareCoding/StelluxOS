#include <klog.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>

namespace klog {
char* logger::m_log_buffer = nullptr;
size_t logger::m_buffer_size = 0;
size_t logger::m_write_index = 0;
mutex logger::m_lock = mutex();

void logger::init(size_t page_count) {
    size_t buffer_size = page_count * PAGE_SIZE;
    m_log_buffer = static_cast<char*>(vmm::alloc_contiguous_virtual_pages(page_count, DEFAULT_UNPRIV_PAGE_FLAGS));

    if (!m_log_buffer) {
        serial::printf("[KLOG] Failed to allocate log buffer.\n");
        return;
    }

    m_buffer_size = buffer_size;
    m_write_index = 0;
    memset(m_log_buffer, 0, m_buffer_size);

    serial::printf("[KLOG] Logger initialized with %llu bytes\n", m_buffer_size);
}

void logger::shutdown() {
    if (m_log_buffer) {
        m_log_buffer = nullptr;
        m_buffer_size = 0;
        m_write_index = 0;
    }
}

size_t logger::read_last_n_lines(size_t n, char* buffer, size_t buffer_size) {
    if (!m_log_buffer || buffer_size == 0) {
        return 0;
    }

    mutex_guard guard(m_lock);

    size_t count = 0;    // Number of newlines found
    size_t index = m_write_index;
    size_t start_index = index;
    
    // Traverse backward to find the last `n` lines
    while (count < n && start_index > 0) {
        start_index--;
        if (m_log_buffer[start_index] == '\n') {
            count++;
        }
    }

    // If we didn’t find enough lines, start from 0
    if (count < n) {
        start_index = 0;
    } else {
        start_index = (start_index + 1) % m_buffer_size; // Move to the first character of the found line
    }

    // Copy the found log lines into `buffer`
    size_t output_index = 0;
    while (start_index != index && output_index < buffer_size - 1) {
        buffer[output_index++] = m_log_buffer[start_index];
        start_index = (start_index + 1) % m_buffer_size;
    }

    buffer[output_index] = '\0'; // Null-terminate the string
    return output_index; // Return the number of characters copied
}

void logger::clear_logs() {
    if (!m_log_buffer) {
        return;
    }

    mutex_guard guard(m_lock);

    memset(m_log_buffer, 0, m_buffer_size);
    m_write_index = 0;
}
} // namespace klog

