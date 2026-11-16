#ifndef MCUFW_FILE_LOCK_HPP
#define MCUFW_FILE_LOCK_HPP

// A whole-file lock that the kernel drops when its holder exits, so a killed mcufw leaves none behind

#include <string>

enum class lock_mode { shared, exclusive };

class file_lock {
public:
    file_lock() = default;
    ~file_lock();

    file_lock(const file_lock&) = delete;
    file_lock& operator=(const file_lock&) = delete;

    // Creates the file when missing, and fails at once, naming the file, when another holder's mode conflicts
    [[nodiscard]] bool acquire(const std::string& path, lock_mode mode, std::string& error);

    // Releases the lock before the object is destroyed
    void release();

private:
    int m_fd = -1;
};

#endif // MCUFW_FILE_LOCK_HPP
