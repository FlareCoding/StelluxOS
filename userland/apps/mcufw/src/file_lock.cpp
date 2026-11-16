#include "file_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

constexpr mode_t LOCK_FILE_MODE = 0644;

file_lock::~file_lock() {
    release();
}

bool file_lock::acquire(const std::string& path, lock_mode mode, std::string& error) {
    release();
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, LOCK_FILE_MODE);
    if (fd < 0) {
        error = "cannot open " + path + ": " + strerror(errno);
        return false;
    }

    if (flock(fd, (mode == lock_mode::shared ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
        int lock_error = errno;
        close(fd);
        error = lock_error == EWOULDBLOCK ? "another mcufw run holds " + path
                                          : "cannot lock " + path + ": " + strerror(lock_error);
        return false;
    }

    m_fd = fd;

    return true;
}

void file_lock::release() {
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}
