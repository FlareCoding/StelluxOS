#ifndef STELLUX_FW_FW_H
#define STELLUX_FW_FW_H

#include "common/types.h"

namespace fs { class file; }

namespace fw {

constexpr int32_t OK                =  0;
constexpr int32_t ERR_NOT_INSTALLED = -1; // the directory the name places the file in does not exist
constexpr int32_t ERR_MISSING       = -2; // that directory exists but holds no file of the name
constexpr int32_t ERR_UNREADABLE    = -3; // what the name leads to is not a regular file, or opening or reading failed
constexpr int32_t ERR_INVALID_ARG   = -4; // a malformed name or out-of-file range, or no file open or one already open

// Where packages install firmware files, the directory every name is relative to
constexpr const char DIRECTORY[] = "/usr/lib/firmware";

/**
 * A firmware file: code or data a driver hands to its device, installed by a
 * package under `DIRECTORY`. A driver opens one by its name relative to
 * `DIRECTORY`, such as "vendor/1.0/device.bin", and the result tells whether a
 * package installed it. The driver then copies ranges of it straight into
 * memory it owns, such as the DMA memory its device reads, so a large file
 * reaches the device with no copy in between. Calls log nothing, since only
 * the caller knows whether a result is a failure, so the caller logs it,
 * naming the file. The object closes its file in `close()` or when it goes out
 * of scope.
 *
 * `read` takes the file's mutex and waits while another thread holds it, so it
 * must not run in an interrupt handler or under a spinlock. The boot thread
 * cannot wait and stops for good on a held mutex, so it may read only a file no
 * other thread uses at the time, as with firmware while drivers attach before
 * userland starts. `fs::init` relies on the same when it unpacks the initrd on
 * that thread. `open`, `size` and `close` never sleep on ramfs, which holds
 * every installed package. None is `__PRIVILEGED_CODE`, and the file system
 * and heap calls they make elevate internally, so any kernel thread may use an
 * object, elevated or lowered. One object must be used by one thread at a
 * time.
 */
class file {
public:
    file() = default;
    ~file();

    file(const file&) = delete;
    file& operator=(const file&) = delete;

    /**
     * @brief Open the firmware file at `name`, which must not be null.
     * @param name Components separated by '/', with no leading or trailing
     * '/'. Each component is 1 to `fs::NAME_MAX` bytes and neither "." nor
     * "..", and `DIRECTORY`, a '/', the name and a NUL fit in `fs::PATH_MAX`.
     * @return `OK` when the object holds the file open. `ERR_NOT_INSTALLED`
     * when the directory the name places the file in does not exist, so no
     * package installed it. `ERR_MISSING` when that directory exists but holds
     * no file of the name. `ERR_UNREADABLE` when the name leads to a directory
     * or anything else that is not a regular file, when a component on the way
     * to the file is not a directory, or when the file system cannot resolve or
     * open the file, as when symbolic links loop or memory runs out.
     * `ERR_INVALID_ARG` for a malformed name, or when the object already holds
     * a file open, which stays open.
     */
    [[nodiscard]] int32_t open(const char* name);

    /**
     * @brief Copy the `count` bytes starting at `offset` into `dst`, which
     * must hold `count` bytes and must not be null.
     * @return `OK` when all `count` bytes were copied, which for a `count` of
     * 0 copies nothing. `ERR_INVALID_ARG` when the object holds no file open,
     * or when `offset` plus `count` exceeds `size()` or overflows, which
     * callers rule out by checking against `size()` first. `ERR_UNREADABLE`
     * when the file system fails or delivers fewer bytes, as when the file
     * shrank after it was opened. On `ERR_UNREADABLE`, `dst` may hold part of
     * the range.
     */
    [[nodiscard]] int32_t read(uint64_t offset, void* dst, size_t count) const;

    // Closes the file. Closing an object that holds no file open does nothing.
    void close();

    // The file's size in bytes when it was opened, or 0 when the object holds no file open
    uint64_t size() const { return m_size; }

private:
    fs::file* m_file = nullptr;
    uint64_t  m_size = 0;
};

} // namespace fw

#endif // STELLUX_FW_FW_H
