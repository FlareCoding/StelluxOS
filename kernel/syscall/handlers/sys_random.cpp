#include "syscall/handlers/sys_random.h"

#include "random/random.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

constexpr uint32_t GRND_NONBLOCK = 0x1;
constexpr size_t GETRANDOM_MAX_BUF = 4096;

DEFINE_SYSCALL3(getrandom, u_buf, buflen, flags) {
    if (u_buf == 0 || buflen == 0) {
        return syscall::EINVAL;
    }

    size_t len = static_cast<size_t>(buflen);
    if (len > GETRANDOM_MAX_BUF) {
        len = GETRANDOM_MAX_BUF;
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(len));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    int32_t rc = random::fill(kbuf, len);
    if (rc != random::OK) {
        heap::kfree(kbuf);
        if (flags & GRND_NONBLOCK) {
            return syscall::EAGAIN;
        }
        return syscall::ENOSYS;
    }

    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_buf), kbuf, len);
    heap::kfree(kbuf);

    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return static_cast<int64_t>(len);
}
