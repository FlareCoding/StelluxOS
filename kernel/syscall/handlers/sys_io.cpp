#include "syscall/handlers/sys_io.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/providers/file_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "fs/fs.h"
#include "fs/file.h"

namespace {

struct iovec {
    uint64_t base;
    uint64_t len;
};

constexpr uint64_t MAX_IOVCNT = 1024;
constexpr size_t IO_CHUNK_SIZE = 4096;

inline int64_t map_resource_error(int64_t rc) {
    switch (rc) {
        case resource::ERR_INVAL:     return syscall::EINVAL;
        case resource::ERR_NOENT:     return syscall::ENOENT;
        case resource::ERR_BADF:
        case resource::ERR_ACCESS:    return syscall::EBADF;
        case resource::ERR_NOMEM:     return syscall::ENOMEM;
        case resource::ERR_TABLEFULL: return syscall::EMFILE;
        case resource::ERR_UNSUP:     return syscall::ENOSYS;
        case resource::ERR_PIPE:      return syscall::EPIPE;
        case resource::ERR_AGAIN:     return syscall::EAGAIN;
        case resource::ERR_IO:
        default:                      return syscall::EIO;
    }
}

} // anonymous namespace

DEFINE_SYSCALL3(readv, fd, iov_ptr, iovcnt) {
    if (iovcnt == 0) return 0;
    if (iovcnt > MAX_IOVCNT) return syscall::EINVAL;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    size_t iov_bytes = static_cast<size_t>(iovcnt) * sizeof(iovec);
    auto* kiovs = static_cast<iovec*>(heap::kzalloc(iov_bytes));
    if (!kiovs) return syscall::ENOMEM;

    int32_t copy_rc = mm::uaccess::copy_from_user(
        kiovs, reinterpret_cast<const iovec*>(iov_ptr), iov_bytes);
    if (copy_rc != mm::uaccess::OK) {
        heap::kfree(kiovs);
        return syscall::EFAULT;
    }

    int64_t total = 0;
    int64_t err = 0;
    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        heap::kfree(kiovs);
        return syscall::ENOMEM;
    }

    bool done = false;
    for (uint64_t i = 0; i < iovcnt && !done; i++) {
        size_t remaining = kiovs[i].len;
        uint8_t* user_ptr = reinterpret_cast<uint8_t*>(kiovs[i].base);

        while (remaining > 0) {
            size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;

            ssize_t n = resource::read(
                task, static_cast<resource::handle_t>(fd), kbuf, chunk);
            if (n < 0) {
                err = map_resource_error(n);
                done = true;
                break;
            }
            if (n == 0) {
                done = true;
                break;
            }

            copy_rc = mm::uaccess::copy_to_user(user_ptr, kbuf, static_cast<size_t>(n));
            if (copy_rc != mm::uaccess::OK) {
                err = syscall::EFAULT;
                done = true;
                break;
            }

            total += n;
            user_ptr += n;
            remaining -= static_cast<size_t>(n);
            if (static_cast<size_t>(n) < chunk) {
                done = true;
                break;
            }
        }
    }

    heap::kfree(kbuf);
    heap::kfree(kiovs);
    return total > 0 ? total : (err ? err : total);
}

DEFINE_SYSCALL3(writev, fd, iov_ptr, iovcnt) {
    if (iovcnt == 0) return 0;
    if (iovcnt > MAX_IOVCNT) return syscall::EINVAL;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    size_t iov_bytes = static_cast<size_t>(iovcnt) * sizeof(iovec);
    auto* kiovs = static_cast<iovec*>(heap::kzalloc(iov_bytes));
    if (!kiovs) return syscall::ENOMEM;

    int32_t copy_rc = mm::uaccess::copy_from_user(
        kiovs, reinterpret_cast<const iovec*>(iov_ptr), iov_bytes);
    if (copy_rc != mm::uaccess::OK) {
        heap::kfree(kiovs);
        return syscall::EFAULT;
    }

    int64_t total = 0;
    int64_t err = 0;
    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        heap::kfree(kiovs);
        return syscall::ENOMEM;
    }

    bool done = false;
    for (uint64_t i = 0; i < iovcnt && !done; i++) {
        size_t remaining = kiovs[i].len;
        const uint8_t* user_ptr =
            reinterpret_cast<const uint8_t*>(kiovs[i].base);

        while (remaining > 0) {
            size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;
            copy_rc = mm::uaccess::copy_from_user(kbuf, user_ptr, chunk);
            if (copy_rc != mm::uaccess::OK) {
                err = syscall::EFAULT;
                done = true;
                break;
            }

            ssize_t n = resource::write(
                task, static_cast<resource::handle_t>(fd), kbuf, chunk);
            if (n < 0) {
                err = map_resource_error(n);
                done = true;
                break;
            }
            if (n == 0) {
                done = true;
                break;
            }

            total += n;
            user_ptr += n;
            remaining -= static_cast<size_t>(n);
            if (static_cast<size_t>(n) < chunk) {
                done = true;
                break;
            }
        }
    }

    heap::kfree(kbuf);
    heap::kfree(kiovs);
    return total > 0 ? total : (err ? err : total);
}

DEFINE_SYSCALL3(ioctl, fd, cmd, arg) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    int32_t rc = resource::ioctl(
        task, static_cast<resource::handle_t>(fd),
        static_cast<uint32_t>(cmd), arg);
    if (rc == resource::OK) return 0;
    return map_resource_error(rc);
}

DEFINE_SYSCALL4(pread64, fd, buf, count, offset) {
    if (count == 0) return 0;
    if (buf == 0) return syscall::EFAULT;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (obj->type != resource::resource_type::FILE) {
        resource::resource_release(obj);
        return syscall::ESPIPE;
    }

    fs::file* f = resource::file_provider::get_file(obj);
    if (!f) {
        resource::resource_release(obj);
        return syscall::EBADF;
    }

    // Save position, seek, read, restore
    int64_t saved_pos = f->offset();
    int64_t seek_rc = fs::seek(f, static_cast<int64_t>(offset), 0); // SEEK_SET
    if (seek_rc < 0) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    size_t remaining = static_cast<size_t>(count);
    uint8_t* user_ptr = reinterpret_cast<uint8_t*>(buf);
    int64_t total = 0;

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        f->set_offset(saved_pos);
        resource::resource_release(obj);
        return syscall::ENOMEM;
    }

    while (remaining > 0) {
        size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;
        ssize_t n = fs::read(f, kbuf, chunk);
        if (n < 0) {
            if (total == 0) total = map_resource_error(n);
            break;
        }
        if (n == 0) break;

        int32_t copy_rc = mm::uaccess::copy_to_user(user_ptr, kbuf, static_cast<size_t>(n));
        if (copy_rc != mm::uaccess::OK) {
            if (total == 0) total = syscall::EFAULT;
            break;
        }

        total += n;
        user_ptr += n;
        remaining -= static_cast<size_t>(n);
        if (static_cast<size_t>(n) < chunk) break;
    }

    heap::kfree(kbuf);
    f->set_offset(saved_pos);
    resource::resource_release(obj);
    return total;
}
