#include "syscall/handlers/sys_fd.h"

#include "resource/resource.h"
#include "resource/providers/file_provider.h"
#include "resource/providers/shmem_resource_provider.h"
#include "resource/providers/shm_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "mm/shmem.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"
#include "common/string.h"

namespace {

constexpr int64_t AT_FDCWD = -100;
constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x100;
constexpr uint64_t AT_NO_AUTOMOUNT = 0x800;
constexpr uint64_t AT_EMPTY_PATH = 0x1000;
constexpr size_t IO_CHUNK_SIZE = 4096;

constexpr uint32_t ST_IFDIR  = 0040000;
constexpr uint32_t ST_IFCHR  = 0020000;
constexpr uint32_t ST_IFBLK  = 0060000;
constexpr uint32_t ST_IFREG  = 0100000;
constexpr uint32_t ST_IFIFO  = 0010000;
constexpr uint32_t ST_IFLNK  = 0120000;
constexpr uint32_t ST_IFSOCK = 0140000;

struct linux_dirent64_hdr {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
} __attribute__((packed));

constexpr uint8_t DT_UNKNOWN = 0;
constexpr uint8_t DT_CHR     = 2;
constexpr uint8_t DT_DIR     = 4;
constexpr uint8_t DT_BLK     = 6;
constexpr uint8_t DT_REG     = 8;
constexpr uint8_t DT_LNK     = 10;
constexpr uint8_t DT_SOCK    = 12;

constexpr size_t GETDENTS64_ALIGN = 8;
constexpr uint16_t GETDENTS64_MIN_RECLEN = static_cast<uint16_t>(
    (sizeof(linux_dirent64_hdr) + 1 + (GETDENTS64_ALIGN - 1)) &
    ~(GETDENTS64_ALIGN - 1));
constexpr uint16_t GETDENTS64_MAX_RECLEN = static_cast<uint16_t>(
    (sizeof(linux_dirent64_hdr) + fs::NAME_MAX + 1 + (GETDENTS64_ALIGN - 1)) &
    ~(GETDENTS64_ALIGN - 1));

#if defined(__x86_64__)
struct linux_kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    int64_t  st_atime_nsec;
    int64_t  st_mtime_sec;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime_sec;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
};
#elif defined(__aarch64__)
struct linux_kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    int64_t  st_atime_nsec;
    int64_t  st_mtime_sec;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime_sec;
    int64_t  st_ctime_nsec;
    uint32_t __unused[2];
};
#endif

inline int64_t map_resource_error(int64_t rc) {
    switch (rc) {
        case resource::ERR_INVAL:
            return syscall::EINVAL;
        case resource::ERR_NOENT:
            return syscall::ENOENT;
        case resource::ERR_NOTDIR:
            return syscall::ENOTDIR;
        case resource::ERR_NAMETOOLONG:
            return syscall::ENAMETOOLONG;
        case resource::ERR_NOMEM:
            return syscall::ENOMEM;
        case resource::ERR_TABLEFULL:
            return syscall::EMFILE;
        case resource::ERR_BADF:
        case resource::ERR_ACCESS:
            return syscall::EBADF;
        case resource::ERR_UNSUP:
            return syscall::ENOSYS;
        case resource::ERR_PIPE:
            return syscall::EPIPE;
        case resource::ERR_NOTCONN:
            return syscall::ENOTCONN;
        case resource::ERR_CONNREFUSED:
            return syscall::ECONNREFUSED;
        case resource::ERR_ADDRINUSE:
            return syscall::EADDRINUSE;
        case resource::ERR_ISCONN:
            return syscall::EISCONN;
        case resource::ERR_AGAIN:
            return syscall::EAGAIN;
        case resource::ERR_EXIST:
            return syscall::EEXIST;
        case resource::ERR_IO:
        default:
            return syscall::EIO;
    }
}

inline int64_t map_fs_error(int32_t rc) {
    switch (rc) {
        case fs::ERR_NOENT:
            return syscall::ENOENT;
        case fs::ERR_EXIST:
            return syscall::EEXIST;
        case fs::ERR_NOTDIR:
            return syscall::ENOTDIR;
        case fs::ERR_ISDIR:
            return syscall::EISDIR;
        case fs::ERR_NOMEM:
            return syscall::ENOMEM;
        case fs::ERR_INVAL:
            return syscall::EINVAL;
        case fs::ERR_NAMETOOLONG:
            return syscall::ENAMETOOLONG;
        case fs::ERR_NOTEMPTY:
            return syscall::ENOTEMPTY;
        case fs::ERR_NOSYS:
            return syscall::ENOSYS;
        case fs::ERR_BUSY:
            return syscall::EBUSY;
        case fs::ERR_LOOP:
            return syscall::ELOOP;
        case fs::ERR_BADF:
            return syscall::EBADF;
        case fs::ERR_IO:
        default:
            return syscall::EIO;
    }
}

inline uint8_t node_type_to_dirent_type(fs::node_type t) {
    switch (t) {
        case fs::node_type::regular:
            return DT_REG;
        case fs::node_type::directory:
            return DT_DIR;
        case fs::node_type::symlink:
            return DT_LNK;
        case fs::node_type::char_device:
            return DT_CHR;
        case fs::node_type::block_device:
            return DT_BLK;
        case fs::node_type::socket:
            return DT_SOCK;
        default:
            return DT_UNKNOWN;
    }
}

inline uint32_t node_type_to_mode_bits(fs::node_type t) {
    switch (t) {
        case fs::node_type::regular:
            return ST_IFREG;
        case fs::node_type::directory:
            return ST_IFDIR;
        case fs::node_type::symlink:
            return ST_IFLNK;
        case fs::node_type::char_device:
            return ST_IFCHR;
        case fs::node_type::block_device:
            return ST_IFBLK;
        case fs::node_type::socket:
            return ST_IFSOCK;
        default:
            return ST_IFIFO;
    }
}

inline uint32_t node_type_default_perms(fs::node_type t) {
    switch (t) {
        case fs::node_type::directory:
            return 0755;
        case fs::node_type::socket:
            return 0666;
        case fs::node_type::char_device:
        case fs::node_type::block_device:
            return 0600;
        case fs::node_type::symlink:
            return 0777;
        default:
            return 0644;
    }
}

inline int64_t copy_stat_to_user(const fs::vattr& attr, uint64_t u_stat) {
    linux_kstat st = {};
    st.st_mode = node_type_to_mode_bits(attr.type) | node_type_default_perms(attr.type);
    st.st_size = static_cast<int64_t>(attr.size);
    st.st_nlink = (attr.type == fs::node_type::directory) ? 2 : 1;
    st.st_blksize = 4096;
    st.st_blocks = static_cast<int64_t>((attr.size + 511) / 512);

    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_stat), &st, sizeof(st));
    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }
    return 0;
}

int64_t do_fstat_common(int64_t fd, uint64_t u_stat) {
    if (u_stat == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    fs::vattr attr = {};
    if (obj->type == resource::resource_type::FILE) {
        fs::file* kfile = resource::file_provider::get_file(obj);
        if (!kfile) {
            resource::resource_release(obj);
            return syscall::EIO;
        }
        int32_t fs_rc = fs::fstat(kfile, &attr);
        resource::resource_release(obj);
        if (fs_rc != fs::OK) {
            return map_fs_error(fs_rc);
        }
        return copy_stat_to_user(attr, u_stat);
    }

    if (obj->type == resource::resource_type::SHMEM) {
        mm::shmem* backing = resource::shmem_resource_provider::get_shmem_backing(obj);
        if (!backing) {
            resource::resource_release(obj);
            return syscall::EINVAL;
        }
        sync::mutex_lock(backing->lock);
        attr.type = fs::node_type::regular;
        attr.size = backing->m_size;
        sync::mutex_unlock(backing->lock);
        resource::resource_release(obj);
        return copy_stat_to_user(attr, u_stat);
    }

    if (obj->type == resource::resource_type::SOCKET) {
        attr.type = fs::node_type::socket;
        attr.size = 0;
        resource::resource_release(obj);
        return copy_stat_to_user(attr, u_stat);
    }

    resource::resource_release(obj);
    return syscall::EINVAL;
}

int64_t do_newfstatat_common(int64_t dirfd, uint64_t pathname, uint64_t u_stat, uint64_t flags) {
    if (u_stat == 0 || pathname == 0) {
        return syscall::EFAULT;
    }

    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH)) != 0) {
        return syscall::EINVAL;
    }

    char kpath[fs::PATH_MAX];
    int32_t copy_rc = mm::uaccess::copy_cstr_from_user(
        kpath, sizeof(kpath), reinterpret_cast<const char*>(pathname));
    if (copy_rc != mm::uaccess::OK) {
        if (copy_rc == mm::uaccess::ERR_NAMETOOLONG) {
            return syscall::ENAMETOOLONG;
        }
        return syscall::EFAULT;
    }

    if (kpath[0] == '\0') {
        if ((flags & AT_EMPTY_PATH) == 0) {
            return syscall::ENOENT;
        }
        if (dirfd < 0) {
            return syscall::EBADF;
        }
        return do_fstat_common(dirfd, u_stat);
    }

    if (kpath[0] != '/') {
        return syscall::EINVAL;
    }
    if (dirfd != AT_FDCWD) {
        return syscall::EINVAL;
    }

    fs::vattr attr = {};
    int32_t fs_rc = fs::stat(kpath, &attr);
    if (fs_rc != fs::OK) {
        return map_fs_error(fs_rc);
    }

    return copy_stat_to_user(attr, u_stat);
}

int64_t do_open_common(int64_t dirfd, uint64_t pathname, uint64_t flags, uint64_t mode) {
    (void)mode;

    if (dirfd != AT_FDCWD) {
        return syscall::EINVAL;
    }

    char kpath[fs::PATH_MAX];
    int32_t copy_rc = mm::uaccess::copy_cstr_from_user(
        kpath,
        sizeof(kpath),
        reinterpret_cast<const char*>(pathname)
    );
    if (copy_rc != mm::uaccess::OK) {
        if (copy_rc == mm::uaccess::ERR_NAMETOOLONG) {
            return syscall::ENAMETOOLONG;
        }
        return syscall::EFAULT;
    }

    if (kpath[0] != '/') {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::handle_t handle = -1;
    int32_t rc = resource::open(
        task,
        kpath,
        static_cast<uint32_t>(flags),
        &handle
    );
    if (rc != resource::OK) {
        return map_resource_error(rc);
    }

    return handle;
}

} // anonymous namespace

DEFINE_SYSCALL4(openat, dirfd, pathname, flags, mode) {
    return do_open_common(static_cast<int64_t>(dirfd), pathname, flags, mode);
}

DEFINE_SYSCALL3(open, pathname, flags, mode) {
    return do_open_common(AT_FDCWD, pathname, flags, mode);
}

DEFINE_SYSCALL3(read, fd, buf, count) {
    if (count == 0) {
        return 0;
    }
    if (buf == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    size_t remaining = static_cast<size_t>(count);
    uint8_t* user_ptr = reinterpret_cast<uint8_t*>(buf);
    int64_t total = 0;

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    while (remaining > 0) {
        size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;
        ssize_t n = resource::read(task, static_cast<resource::handle_t>(fd), kbuf, chunk);
        if (n < 0) {
            heap::kfree(kbuf);
            if (total > 0) {
                return total;
            }
            return map_resource_error(n);
        }
        if (n == 0) {
            break;
        }

        int32_t rc = mm::uaccess::copy_to_user(user_ptr, kbuf, static_cast<size_t>(n));
        if (rc != mm::uaccess::OK) {
            heap::kfree(kbuf);
            if (total > 0) {
                return total;
            }
            return syscall::EFAULT;
        }

        total += n;
        user_ptr += n;
        remaining -= static_cast<size_t>(n);

        if (static_cast<size_t>(n) < chunk) {
            break;
        }
    }

    heap::kfree(kbuf);
    return total;
}

DEFINE_SYSCALL3(write, fd, buf, count) {
    if (count == 0) {
        return 0;
    }
    if (buf == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    size_t remaining = static_cast<size_t>(count);
    const uint8_t* user_ptr = reinterpret_cast<const uint8_t*>(buf);
    int64_t total = 0;

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    while (remaining > 0) {
        size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;
        int32_t copy_rc = mm::uaccess::copy_from_user(kbuf, user_ptr, chunk);
        if (copy_rc != mm::uaccess::OK) {
            heap::kfree(kbuf);
            if (total > 0) {
                return total;
            }
            return syscall::EFAULT;
        }

        ssize_t n = resource::write(task, static_cast<resource::handle_t>(fd), kbuf, chunk);
        if (n < 0) {
            heap::kfree(kbuf);
            if (total > 0) {
                return total;
            }
            return map_resource_error(n);
        }
        if (n == 0) {
            break;
        }

        total += n;
        user_ptr += n;
        remaining -= static_cast<size_t>(n);

        if (static_cast<size_t>(n) < chunk) {
            break;
        }
    }

    heap::kfree(kbuf);
    return total;
}

DEFINE_SYSCALL1(close, fd) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    int32_t rc = resource::close(task, static_cast<resource::handle_t>(fd));
    if (rc != resource::OK) {
        return map_resource_error(rc);
    }
    return 0;
}

DEFINE_SYSCALL2(stat, pathname, statbuf) {
    return do_newfstatat_common(AT_FDCWD, pathname, statbuf, 0);
}

DEFINE_SYSCALL2(fstat, fd, statbuf) {
    return do_fstat_common(static_cast<int64_t>(fd), statbuf);
}

DEFINE_SYSCALL4(newfstatat, dirfd, pathname, statbuf, flags) {
    return do_newfstatat_common(
        static_cast<int64_t>(dirfd), pathname, statbuf, flags);
}

DEFINE_SYSCALL3(getdents64, fd, dirp, count) {
    if (dirp == 0) {
        return syscall::EFAULT;
    }
    if (count == 0) {
        return 0;
    }
    if (count > 0xFFFFFFFFULL) {
        return syscall::EINVAL;
    }

    uint32_t out_cap = static_cast<uint32_t>(count);
    if (out_cap < GETDENTS64_MIN_RECLEN) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) {
        return (rc == resource::HANDLE_ERR_ACCESS) ? syscall::EBADF : syscall::EBADF;
    }

    if (obj->type != resource::resource_type::FILE) {
        resource::resource_release(obj);
        return syscall::ENOTDIR;
    }

    fs::file* kfile = resource::file_provider::get_file(obj);
    if (!kfile || !kfile->get_node()) {
        resource::resource_release(obj);
        return syscall::EIO;
    }

    if (kfile->get_node()->type() != fs::node_type::directory) {
        resource::resource_release(obj);
        return syscall::ENOTDIR;
    }

    uint8_t record_buf[GETDENTS64_MAX_RECLEN];
    uint32_t bytes_written = 0;

    while (bytes_written < out_cap) {
        uint32_t remaining = out_cap - bytes_written;
        if (remaining < GETDENTS64_MIN_RECLEN) {
            break;
        }

        int64_t offset_before = kfile->offset();
        fs::dirent entry = {};
        ssize_t nread = fs::readdir(kfile, &entry, 1);
        if (nread < 0) {
            resource::resource_release(obj);
            if (bytes_written > 0) {
                return static_cast<int64_t>(bytes_written);
            }
            return map_fs_error(static_cast<int32_t>(nread));
        }
        if (nread == 0) {
            break;
        }

        size_t name_len = string::strnlen(entry.name, fs::NAME_MAX);
        uint16_t reclen = static_cast<uint16_t>(
            (sizeof(linux_dirent64_hdr) + name_len + 1 + (GETDENTS64_ALIGN - 1)) &
            ~(GETDENTS64_ALIGN - 1));

        if (reclen > remaining) {
            kfile->set_offset(offset_before);
            if (bytes_written == 0) {
                resource::resource_release(obj);
                return syscall::EINVAL;
            }
            break;
        }

        string::memset(record_buf, 0, reclen);
        linux_dirent64_hdr hdr = {};
        hdr.d_ino = 0;
        hdr.d_off = kfile->offset();
        hdr.d_reclen = reclen;
        hdr.d_type = node_type_to_dirent_type(entry.type);

        string::memcpy(record_buf, &hdr, sizeof(hdr));
        string::memcpy(record_buf + sizeof(hdr), entry.name, name_len);
        record_buf[sizeof(hdr) + name_len] = '\0';

        int32_t copy_rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(dirp + bytes_written),
            record_buf, reclen);
        if (copy_rc != mm::uaccess::OK) {
            kfile->set_offset(offset_before);
            resource::resource_release(obj);
            if (bytes_written > 0) {
                return static_cast<int64_t>(bytes_written);
            }
            return syscall::EFAULT;
        }

        bytes_written += reclen;
    }

    resource::resource_release(obj);
    return static_cast<int64_t>(bytes_written);
}

namespace {
constexpr uint64_t F_GETFL = 3;
constexpr uint64_t F_SETFL = 4;
constexpr uint32_t SETFL_MASK = fs::O_NONBLOCK | fs::O_APPEND;
} // anonymous namespace

DEFINE_SYSCALL3(fcntl, fd, cmd, arg) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    if (cmd == F_GETFL) {
        uint32_t flags = 0;
        int32_t rc = resource::get_handle_flags(
            &task->handles, static_cast<resource::handle_t>(fd), &flags);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }
        return static_cast<int64_t>(flags);
    }

    if (cmd == F_SETFL) {
        uint32_t flags = static_cast<uint32_t>(arg) & SETFL_MASK;
        int32_t rc = resource::set_handle_flags(
            &task->handles, static_cast<resource::handle_t>(fd), flags);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }
        return 0;
    }

    return syscall::EINVAL;
}

DEFINE_SYSCALL3(unlinkat, dirfd, pathname, flags_val) {
    (void)flags_val;

    if (static_cast<int64_t>(dirfd) != AT_FDCWD) {
        return syscall::EINVAL;
    }

    char kpath[fs::PATH_MAX];
    int32_t copy_rc = mm::uaccess::copy_cstr_from_user(
        kpath, sizeof(kpath),
        reinterpret_cast<const char*>(pathname));
    if (copy_rc != mm::uaccess::OK) {
        if (copy_rc == mm::uaccess::ERR_NAMETOOLONG) {
            return syscall::ENAMETOOLONG;
        }
        return syscall::EFAULT;
    }

    if (kpath[0] != '/') {
        return syscall::EINVAL;
    }

    if (resource::shm_provider::is_shm_path(kpath)) {
        int32_t rc = resource::shm_provider::unlink_shm(kpath);
        if (rc != resource::OK) {
            return map_resource_error(rc);
        }
        return 0;
    }

    int32_t rc = fs::unlink(kpath);
    if (rc != fs::OK) {
        return map_fs_error(rc);
    }
    return 0;
}
