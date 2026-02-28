#include "syscall/handlers/sys_io.h"
#include "io/serial.h"

namespace {

struct iovec {
    uint64_t base;
    uint64_t len;
};

constexpr uint64_t MAX_IOVCNT = 1024;

} // anonymous namespace

DEFINE_SYSCALL3(writev, fd, iov_ptr, iovcnt) {
    if (iovcnt == 0) return 0;
    if (iovcnt > MAX_IOVCNT) return syscall::EINVAL;

    auto* iov = reinterpret_cast<const iovec*>(iov_ptr);
    int64_t total = 0;

    for (uint64_t i = 0; i < iovcnt; i++) {
        auto* buf = reinterpret_cast<const char*>(iov[i].base);
        size_t len = iov[i].len;
        if (len == 0) {
            continue;
        }

        if (fd == 1 || fd == 2) {
            serial::write(buf, len);
        } else {
            return syscall::EBADF;
        }

        total += static_cast<int64_t>(len);
    }

    return total;
}

DEFINE_SYSCALL0(ioctl) {
    return syscall::ENOTTY;
}
