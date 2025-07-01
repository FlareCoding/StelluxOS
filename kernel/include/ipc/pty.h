#ifndef PTY_H
#define PTY_H
#include <core/sync.h>
#include <kstl/ring_buffer.h>
#include <kstl/vector.h>

/**
 * @enum pty_input_policy
 * @brief Defines the input processing policy for a PTY.
 *
 * This policy is typically applied on the receiving side to process
 * input coming from the peer (e.g., a terminal emulator).
 */
enum class pty_input_policy {
    RAW,    // Raw mode: pass all characters through without processing.
    COOKED  // Cooked mode: provides line-editing features (e.g., backspace) and echoing.
};

/**
 * @class pty
 * @brief Pseudo-terminal (PTY) implementation.
 * 
 * Provides a bidirectional communication channel between a master and slave
 * terminal device. The master side typically connects to a terminal emulator,
 * while the slave side connects to a process that thinks it's talking to a 
 * real terminal.
 * 
 * In cooked mode, the receiving side provides line editing and echoing:
 * - Backspace (^H) removes the last character
 * - Delete (^?) removes the last character
 * - ^U clears the current line
 * - ^C/^D/^Z are passed through as signals
 * - Other control characters are handled according to POSIX terminal semantics
 */
class pty {
public:
    /**
     * @brief Constructs a new PTY device.
     * @param id Unique identifier for this PTY
     * @param policy Input processing policy (raw or cooked)
     */
    pty(uint32_t id, pty_input_policy policy = pty_input_policy::RAW)
        : m_id(id), m_peer_pty(nullptr), m_input_policy(policy), m_buffer(4096) {}
    
    /**
     * @brief Destructor. Ensures proper cleanup of resources.
     */
    ~pty() = default;

    /**
     * @brief Reads data from the PTY.
     * @param buffer Destination buffer
     * @param count Maximum number of bytes to read
     * @return Number of bytes read, or negative error code
     * 
     * In blocking mode, waits until data is available.
     * In non-blocking mode, returns -EAGAIN if no data is available.
     */
    ssize_t read(void* buffer, size_t count);
    
    /**
     * @brief Writes data to the PTY.
     * @param data Source buffer
     * @param count Number of bytes to write
     * @return Number of bytes written, or negative error code
     * 
     * Data written to a PTY is sent to its peer. If the peer is in cooked mode,
     * input processing is applied (line editing, echoing).
     */
    ssize_t write(const void* data, size_t count);

    /**
     * @brief Closes the PTY device.
     * 
     * Flushes buffers and marks the device as closed. Further reads will
     * return EOF, writes will return error.
     */
    void close();

    /**
     * @brief Checks if data is available to read.
     * @return true if data can be read without blocking
     */
    bool is_data_available() const;

    /**
     * @brief Sets blocking mode for read operations.
     * @param blocking true for blocking mode, false for non-blocking
     */
    void set_blocking(bool blocking) { m_blocking = blocking; }

    /**
     * @brief Gets current blocking mode.
     * @return true if in blocking mode
     */
    bool is_blocking() const { return m_blocking; }

    /**
     * @brief Sets the peer PTY device.
     * @param peer Pointer to the peer PTY
     */
    inline void set_peer_pty(pty* peer) { m_peer_pty = peer; }

    /**
     * @brief Gets the peer PTY device.
     * @return Pointer to the peer PTY
     */
    inline pty* get_peer_pty() const { return m_peer_pty; }

    /**
     * @brief Sets the input processing policy.
     * @param policy New input policy
     */
    inline void set_input_policy(pty_input_policy policy) { m_input_policy = policy; }

    /**
     * @brief Gets the current input processing policy.
     * @return Current input policy
     */
    inline pty_input_policy get_input_policy() const { return m_input_policy; }

    /**
     * @brief Gets the PTY identifier.
     * @return PTY ID
     */
    inline uint32_t get_id() const { return m_id; }

private:
    /**
     * @brief Process received data according to input policy.
     * @param data Data to process
     * @param count Number of bytes
     * @return Number of bytes processed
     */
    ssize_t _process_received_data(const void* data, size_t count);

    /**
     * @brief Process a single character in cooked mode.
     * @param c Character to process
     * @return true if character was consumed by line editing
     */
    bool _process_cooked_char(char c);

    /**
     * @brief Echo a character back to the peer.
     * @param c Character to echo
     */
    void _echo_char(char c);

    /**
     * @brief Flush the line buffer to the ring buffer.
     * @return Number of bytes flushed, or negative error code
     */
    ssize_t _flush_line_buffer();

    /**
     * @brief Handle special control characters.
     * @param c Character to check
     * @return true if character was a special control char
     */
    bool _handle_control_char(char c);

    /**
     * @brief Write directly to the ring buffer.
     * @param data Data to write
     * @param count Number of bytes
     * @return Number of bytes written
     */
    ssize_t _write_to_buffer(const void* data, size_t count);

    // Core PTY state
    uint32_t m_id = 0;                    // Unique PTY identifier
    bool m_blocking = true;               // Blocking mode flag
    bool m_closed = false;                // Closed state flag
    pty* m_peer_pty = nullptr;            // Peer PTY device
    
    // Input processing state
    pty_input_policy m_input_policy;      // Current input policy
    kstl::vector<char> m_line_buffer;     // Line buffer for cooked mode
    bool m_echo_enabled = true;           // Echo flag for cooked mode

    // Data buffer with atomic operations
    kstl::byte_ring_buffer m_buffer;      // Main data buffer

    // Synchronization locks
    mutex m_read_mutex = mutex();         // Protects read operations
    mutex m_write_mutex = mutex();        // Protects write operations
};

#endif // PTY_H

