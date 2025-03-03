#ifndef KLOG_H
#define KLOG_H
#include <types.h>
#include <sync.h>
#include <serial/serial.h>

namespace klog {
class logger {
public:
    /**
     * @brief Initializes the kernel log system.
     * 
     * This must be called after memory allocators are initialized since it allocates
     * the ring buffer for logs.
     *
     * @param page_count Number of pages to allocate for the log buffer.
     */
    static void init(size_t page_count);

    /**
     * @brief Shuts down the kernel logger.
     * 
     * This function clears the log buffer and releases allocated memory.
     */
    static void shutdown();

    /**
     * @brief Logs a message to the kernel log buffer and serial output.
     * 
     * This function writes a formatted log message into the ring buffer and immediately
     * flushes it to the serial console.
     *
     * @param format The format string.
     * @param args The arguments for formatting.
     */
    template <typename... Args>
    static void log(const char* format, Args... args) {
        if (!m_log_buffer) {
            return;
        }

        constexpr size_t BUFFER_SIZE = 256;
        char temp_buffer[BUFFER_SIZE] = {0};

        // Format the log message
        int len = sprintf(temp_buffer, BUFFER_SIZE, format, args...);
        if (len <= 0) {
            return;
        }

        mutex_guard guard(m_lock);

        // Write to ring buffer
        for (int i = 0; i < len; ++i) {
            m_log_buffer[m_write_index] = temp_buffer[i];
            m_write_index = (m_write_index + 1) % m_buffer_size;
        }

        // Always flush to serial immediately
        serial::printf("%s", temp_buffer);
    }

    /**
     * @brief Reads the last `n` lines from the log buffer.
     *
     * This method extracts up to `n` lines from the end of the log buffer.
     * Useful for displaying logs on a screen.
     *
     * @param n The number of lines to retrieve.
     * @param buffer The destination buffer to store the retrieved lines.
     * @param buffer_size The maximum size of the destination buffer.
     * @return size_t The number of bytes written to `buffer`.
     */
    static size_t read_last_n_lines(size_t n, char* buffer, size_t buffer_size);

    /**
     * @brief Clears all logs from the buffer.
     */
    static void clear_logs();

private:
    static char* m_log_buffer;  // Circular log buffer
    static size_t m_buffer_size;
    static size_t m_write_index;
    static mutex m_lock;        // Synchronization
};
} // namespace klog

#define kprint klog::logger::log

#endif // KLOG_H

