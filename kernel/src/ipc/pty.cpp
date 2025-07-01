#include <ipc/pty.h>
#include <core/klog.h>
#include <syscall/syscalls.h>
#include <sched/sched.h>

// Special control characters
#define CHAR_BACKSPACE   0x08  // ^H
#define CHAR_DELETE      0x7F  // ^?
#define CHAR_CTRL_U      0x15  // ^U
#define CHAR_CTRL_C      0x03  // ^C
#define CHAR_CTRL_D      0x04  // ^D
#define CHAR_CTRL_Z      0x1A  // ^Z
#define CHAR_NEWLINE     0x0A  // \n
#define CHAR_CARRIAGE    0x0D  // \r

ssize_t pty::write(const void* data, size_t count) {
    if (m_closed) {
        return -EIO;
    }

    mutex_guard guard(m_write_mutex);

    if (!m_peer_pty) {
        return -EIO;
    }

    // Send data to peer - it will handle input processing based on its policy
    return m_peer_pty->_process_received_data(data, count);
}

ssize_t pty::read(void* buffer, size_t count) {
    if (m_closed) {
        return 0; // EOF
    }

    if (!buffer || count == 0) {
        return -EINVAL;
    }

    mutex_guard guard(m_read_mutex);

    if (m_blocking) {
        // Blocking read - wait for data
        while (!is_data_available() && !m_closed) {
            // Yield to avoid burning CPU while waiting
            sched::yield();
        }

        if (m_closed) {
            return 0; // EOF
        }
    } else {
        // Non-blocking read
        if (!is_data_available()) {
            return -EAGAIN;
        }
    }

    // Read from ring buffer
    return m_buffer.read_bulk(reinterpret_cast<uint8_t*>(buffer), count);
}

void pty::close() {
    mutex_guard read_guard(m_read_mutex);
    mutex_guard write_guard(m_write_mutex);

    if (m_closed) {
        return;
    }

    // Flush any remaining data in cooked mode
    if (m_input_policy == pty_input_policy::COOKED) {
        _flush_line_buffer();
    }

    m_closed = true;
    m_peer_pty = nullptr;
}

bool pty::is_data_available() const {
    return !m_buffer.empty();
}

ssize_t pty::_process_received_data(const void* data, size_t count) {
    if (!data || count == 0) {
        return -EINVAL;
    }

    if (m_input_policy == pty_input_policy::COOKED) {
        // Process each character in cooked mode
        const char* chars = reinterpret_cast<const char*>(data);
        size_t processed = 0;

        for (size_t i = 0; i < count; i++) {
            _process_cooked_char(chars[i]);
            processed++;
        }

        return processed;
    } else {
        // Raw mode - write directly to buffer
        return _write_to_buffer(data, count);
    }
}

bool pty::_process_cooked_char(char c) {
    switch (c) {
        case CHAR_BACKSPACE:
        case CHAR_DELETE: {
            if (!m_line_buffer.empty()) {
                if (m_echo_enabled) {
                    // Echo backspace sequence
                    const char bs_seq[] = {CHAR_BACKSPACE, ' ', CHAR_BACKSPACE};
                    if (m_peer_pty) {
                        m_peer_pty->_process_received_data(bs_seq, sizeof(bs_seq));
                    }
                }
                m_line_buffer.pop_back();
            }
            return true;
        }

        case CHAR_CTRL_U: {
            if (m_echo_enabled) {
                // Clear line: CR + spaces + CR
                size_t len = m_line_buffer.size();
                if (m_peer_pty && len > 0) {
                    char* clear_seq = new char[len + 3];
                    clear_seq[0] = '\r';
                    memset(clear_seq + 1, ' ', len);
                    clear_seq[len + 1] = '\r';
                    clear_seq[len + 2] = '\0';
                    m_peer_pty->_process_received_data(clear_seq, len + 2);
                    delete[] clear_seq;
                }
            }
            m_line_buffer.clear();
            return true;
        }

        case CHAR_NEWLINE:
        case CHAR_CARRIAGE: {
            m_line_buffer.push_back(CHAR_NEWLINE);
            if (m_echo_enabled) {
                _echo_char(CHAR_NEWLINE);
            }
            _flush_line_buffer();
            return true;
        }

        default: {
            if (_handle_control_char(c)) {
                // Control character was handled
                return true;
            }

            // Regular character - add to line buffer
            m_line_buffer.push_back(c);
            if (m_echo_enabled) {
                _echo_char(c);
            }
            return false;
        }
    }
}

void pty::_echo_char(char c) {
    if (m_peer_pty) {
        m_peer_pty->_process_received_data(&c, 1);
    }
}

ssize_t pty::_flush_line_buffer() {
    if (m_line_buffer.empty()) {
        return 0;
    }

    ssize_t written = _write_to_buffer(m_line_buffer.data(), m_line_buffer.size());
    if (written > 0) {
        m_line_buffer.clear();
    }
    return written;
}

bool pty::_handle_control_char(char c) {
    switch (c) {
        case CHAR_CTRL_C:
        case CHAR_CTRL_D:
        case CHAR_CTRL_Z:
            // Pass through control characters directly
            _write_to_buffer(&c, 1);
            return true;
        default:
            return false;
    }
}

ssize_t pty::_write_to_buffer(const void* data, size_t count) {
    if (!data || count == 0) {
        return -EINVAL;
    }

    // Write directly to ring buffer
    return m_buffer.write_bulk(reinterpret_cast<const uint8_t*>(data), count);
}
