#include "syscall/handlers/sys_fd.h"

#include "resource/resource.h"
#include "resource/providers/file_provider.h"
#include "resource/providers/shmem_provider.h"
#include "resource/providers/shm_provider.h"
#include "syscall/handlers/sys_dup.h"
#include "syscall/handlers/sys_error_map.h"
#include "syscall/handlers/sys_io.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "mm/shmem.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"
#include "fs/record_lock_table.h"
#include "clock/clock.h"
#include "common/string.h"

constexpr int64_t AT_FDCWD = -100;
constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x100;
constexpr uint32_t AT_REMOVEDIR = 0x200;
constexpr uint64_t AT_NO_AUTOMOUNT = 0x800;
constexpr uint64_t AT_EMPTY_PATH = 0x1000;

// Staged transfers given this offset use and advance the file's own offset
constexpr int64_t CURRENT_FILE_OFFSET = -1;

using staged_transfer_fn = int64_t (*)(
    sched::task* task, uint64_t fd, uint64_t buf, uint64_t count, int64_t offset, uint32_t call_flags);

// utimensat tv_nsec values that pick the current time or leave a stamp alone
constexpr int64_t UTIME_NOW  = 0x3fffffff;
constexpr int64_t UTIME_OMIT = 0x3ffffffe;

constexpr uint32_t ST_IFDIR  = 0040000;
constexpr uint32_t ST_IFCHR  = 0020000;
constexpr uint32_t ST_IFBLK  = 0060000;
constexpr uint32_t ST_IFREG  = 0100000;
constexpr uint32_t ST_IFLNK  = 0120000;
constexpr uint32_t ST_IFSOCK = 0140000;

constexpr uint8_t DT_UNKNOWN = 0;
constexpr uint8_t DT_CHR     = 2;
constexpr uint8_t DT_DIR     = 4;
constexpr uint8_t DT_BLK     = 6;
constexpr uint8_t DT_REG     = 8;
constexpr uint8_t DT_LNK     = 10;
constexpr uint8_t DT_SOCK    = 12;

constexpr uint64_t NS_PER_SEC = 1000000000ULL;

namespace {

struct linux_dirent64_hdr {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
} __attribute__((packed));

struct kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_flock {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
};

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

} // anonymous namespace

constexpr size_t GETDENTS64_ALIGN = 8;
constexpr uint16_t GETDENTS64_MIN_RECLEN = static_cast<uint16_t>(
    (sizeof(linux_dirent64_hdr) + 1 + (GETDENTS64_ALIGN - 1)) &
    ~(GETDENTS64_ALIGN - 1));
constexpr uint16_t GETDENTS64_MAX_RECLEN = static_cast<uint16_t>(
    (sizeof(linux_dirent64_hdr) + fs::NAME_MAX + 1 + (GETDENTS64_ALIGN - 1)) &
    ~(GETDENTS64_ALIGN - 1));

static inline int64_t map_resource_error(int64_t rc) {
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
        case resource::ERR_INTR:
            return syscall::ERESTARTSYS;
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
        case resource::ERR_LOOP:
            return syscall::ELOOP;
        case resource::ERR_SPIPE:
            return syscall::ESPIPE;
        case resource::ERR_IO:
        default:
            return syscall::EIO;
    }
}

static inline uint8_t node_type_to_dirent_type(fs::node_type t) {
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

static inline uint32_t node_type_to_mode_bits(fs::node_type t) {
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
            return 0;
    }
}

static inline uint32_t node_type_default_perms(fs::node_type t) {
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

static inline int64_t copy_stat_to_user(const fs::vattr& attr, uint64_t u_stat) {
    linux_kstat st = {};
    st.st_dev = attr.dev;
    st.st_ino = attr.ino;
    st.st_mode = node_type_to_mode_bits(attr.type) | node_type_default_perms(attr.type);
    st.st_nlink = (attr.type == fs::node_type::directory) ? 2 : 1;

    st.st_size = static_cast<int64_t>(attr.size);
    st.st_blksize = 4096;
    st.st_blocks = static_cast<int64_t>((attr.size + 511) / 512);

    st.st_atime_sec = static_cast<int64_t>(attr.atime_ns / NS_PER_SEC);
    st.st_atime_nsec = static_cast<int64_t>(attr.atime_ns % NS_PER_SEC);
    st.st_mtime_sec = static_cast<int64_t>(attr.mtime_ns / NS_PER_SEC);
    st.st_mtime_nsec = static_cast<int64_t>(attr.mtime_ns % NS_PER_SEC);
    st.st_ctime_sec = static_cast<int64_t>(attr.ctime_ns / NS_PER_SEC);
    st.st_ctime_nsec = static_cast<int64_t>(attr.ctime_ns % NS_PER_SEC);

    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_stat), &st, sizeof(st));
    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}

static inline void release_node_ref(fs::node* n) {
    if (!n) {
        return;
    }

    if (n->release()) {
        fs::node::ref_destroy(n);
    }
}

static int64_t acquire_task_cwd_node(sched::task* task, fs::node** out_node) {
    if (!task || !out_node) {
        return syscall::EINVAL;
    }

    if (task->cwd) {
        task->cwd->add_ref();
        *out_node = task->cwd;
        return 0;
    }

    fs::node* root = nullptr;
    int32_t fs_rc = fs::lookup("/", &root);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    *out_node = root;
    return 0;
}

static int64_t replace_task_cwd_node(sched::task* task, fs::node* new_cwd) {
    if (!task || !new_cwd) {
        return syscall::EINVAL;
    }

    if (new_cwd->type() != fs::node_type::directory) {
        return syscall::ENOTDIR;
    }

    new_cwd->add_ref();
    fs::node* old = task->cwd;
    task->cwd = new_cwd;
    release_node_ref(old);
    return 0;
}

static int64_t resolve_dirfd_base_node(
    sched::task* task, int64_t dirfd, fs::node** out_base
) {
    if (!task || !out_base) {
        return syscall::EINVAL;
    }

    if (dirfd == AT_FDCWD) {
        return acquire_task_cwd_node(task, out_base);
    }

    if (dirfd < 0) {
        return syscall::EBADF;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(dirfd), 0, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
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

    fs::node* dir = kfile->get_node();
    if (dir->type() != fs::node_type::directory) {
        resource::resource_release(obj);
        return syscall::ENOTDIR;
    }

    dir->add_ref();
    resource::resource_release(obj);
    *out_base = dir;
    return 0;
}

static int64_t normalize_absolute_path(
    const char* base_abs, const char* input_path,
    char* out_path, size_t out_cap
) {
    if (!input_path || !out_path || out_cap < 2) {
        return syscall::EINVAL;
    }

    out_path[0] = '/';
    out_path[1] = '\0';
    size_t out_len = 1;

    auto pop_component = [&]() {
        if (out_len <= 1) {
            return;
        }

        while (out_len > 1 && out_path[out_len - 1] == '/') {
            out_len--;
        }
        while (out_len > 1 && out_path[out_len - 1] != '/') {
            out_len--;
        }
        if (out_len > 1 && out_path[out_len - 1] == '/') {
            out_len--;
        }

        if (out_len == 0) {
            out_len = 1;
            out_path[0] = '/';
        }
        out_path[out_len] = '\0';
    };

    auto append_component = [&](const char* src, size_t len) -> int64_t {
        if (len > fs::NAME_MAX) {
            return syscall::ENAMETOOLONG;
        }

        if (out_len > 1) {
            if (out_len + 1 >= out_cap) {
                return syscall::ENAMETOOLONG;
            }

            out_path[out_len++] = '/';
            out_path[out_len] = '\0';
        }

        if (out_len + len >= out_cap) {
            return syscall::ENAMETOOLONG;
        }

        string::memcpy(out_path + out_len, src, len);
        out_len += len;
        out_path[out_len] = '\0';
        return 0;
    };

    auto consume = [&](const char* src) -> int64_t {
        size_t pos = 0;
        while (src[pos] != '\0') {
            while (src[pos] == '/') {
                pos++;
            }
            if (src[pos] == '\0') {
                break;
            }

            size_t start = pos;
            while (src[pos] != '\0' && src[pos] != '/') {
                pos++;
            }
            size_t len = pos - start;
            if (len == 0 || (len == 1 && src[start] == '.')) {
                continue;
            }

            if (len == 2 && src[start] == '.' && src[start + 1] == '.') {
                pop_component();
                continue;
            }

            int64_t append_rc = append_component(src + start, len);
            if (append_rc != 0) {
                return append_rc;
            }
        }
        return 0;
    };

    if (input_path[0] != '/') {
        if (!base_abs || base_abs[0] != '/') {
            return syscall::EINVAL;
        }

        int64_t base_rc = consume(base_abs);
        if (base_rc != 0) {
            return base_rc;
        }
    }

    int64_t path_rc = consume(input_path);
    if (path_rc != 0) {
        return path_rc;
    }

    return 0;
}

static int64_t normalize_path_for_dirfd(
    sched::task* task, int64_t dirfd,
    const char* input_path, char* out_path, size_t out_cap
) {
    if (!task || !input_path || !out_path || out_cap == 0) {
        return syscall::EINVAL;
    }

    if (input_path[0] == '/') {
        return normalize_absolute_path(nullptr, input_path, out_path, out_cap);
    }

    fs::node* base_node = nullptr;
    int64_t base_rc = resolve_dirfd_base_node(task, dirfd, &base_node);
    if (base_rc != 0) {
        return base_rc;
    }

    char* base_path = static_cast<char*>(heap::uzalloc(fs::PATH_MAX));
    if (!base_path) {
        release_node_ref(base_node);
        return syscall::ENOMEM;
    }

    int32_t base_path_rc = fs::path_from_node(base_node, base_path, fs::PATH_MAX);
    release_node_ref(base_node);
    if (base_path_rc != fs::OK) {
        heap::ufree(base_path);
        return syscall::error_map::map_fs_error(base_path_rc);
    }

    int64_t norm_rc = normalize_absolute_path(base_path, input_path, out_path, out_cap);
    heap::ufree(base_path);
    return norm_rc;
}

static int64_t lookup_node_for_dirfd_path(
    sched::task* task,
    int64_t dirfd,
    const char* input_path,
    fs::node** out_node,
    uint32_t lookup_flags = 0
) {
    if (!task || !input_path || !out_node) {
        return syscall::EINVAL;
    }

    fs::node* base = nullptr;
    if (input_path[0] != '/') {
        int64_t base_rc = resolve_dirfd_base_node(task, dirfd, &base);
        if (base_rc != 0) {
            return base_rc;
        }
    }

    int32_t fs_rc = fs::lookup_at(base, input_path, lookup_flags, out_node);
    release_node_ref(base);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    return 0;
}

static int64_t resolve_parent_for_dirfd_path(
    sched::task* task,
    int64_t dirfd,
    const char* input_path,
    fs::node** out_parent,
    const char** out_name,
    size_t* out_name_len
) {
    if (!task || !input_path || !out_parent || !out_name || !out_name_len) {
        return syscall::EINVAL;
    }

    fs::node* base = nullptr;
    if (input_path[0] != '/') {
        int64_t base_rc = resolve_dirfd_base_node(task, dirfd, &base);
        if (base_rc != 0) {
            return base_rc;
        }
    }

    int32_t fs_rc = fs::resolve_parent_path_at(
        base, input_path, out_parent, out_name, out_name_len);
    release_node_ref(base);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    return 0;
}

static int64_t resolve_open_resource_path(
    sched::task* task,
    int64_t dirfd,
    const char* input_path,
    uint32_t open_flags,
    char* out_path,
    size_t out_cap
) {
    if (!task || !input_path || !out_path || out_cap < 2) {
        return syscall::EINVAL;
    }

    if ((open_flags & fs::O_CREAT) != 0) {
        fs::node* parent = nullptr;
        const char* name = nullptr;
        size_t name_len = 0;
        int64_t parent_rc = resolve_parent_for_dirfd_path(
            task, dirfd, input_path, &parent, &name, &name_len);
        if (parent_rc != 0) {
            return parent_rc;
        }

        int32_t parent_path_rc = fs::path_from_node(parent, out_path, out_cap);
        release_node_ref(parent);
        if (parent_path_rc != fs::OK) {
            return syscall::error_map::map_fs_error(parent_path_rc);
        }

        size_t parent_len = string::strnlen(out_path, out_cap);
        bool need_sep = !(parent_len == 1 && out_path[0] == '/');
        size_t needed = parent_len + (need_sep ? 1 : 0) + name_len + 1;
        if (needed > out_cap) {
            return syscall::ENAMETOOLONG;
        }

        size_t pos = parent_len;
        if (need_sep) {
            out_path[pos++] = '/';
        }
        string::memcpy(out_path + pos, name, name_len);
        out_path[pos + name_len] = '\0';
        return 0;
    }

    fs::node* target = nullptr;
    int64_t lookup_rc = lookup_node_for_dirfd_path(task, dirfd, input_path, &target);
    if (lookup_rc != 0) {
        return lookup_rc;
    }

    int32_t path_rc = fs::path_from_node(target, out_path, out_cap);
    release_node_ref(target);
    if (path_rc != fs::OK) {
        return syscall::error_map::map_fs_error(path_rc);
    }

    return 0;
}

static int64_t do_fstat_common(int64_t fd, uint64_t u_stat) {
    if (u_stat == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
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
            return syscall::error_map::map_fs_error(fs_rc);
        }

        return copy_stat_to_user(attr, u_stat);
    }

    if (obj->type == resource::resource_type::SHMEM) {
        mm::shmem* backing = resource::shmem_provider::get_shmem_backing(obj);
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

// Turns the utimensat times array into a setattr request: a null array
// means now for both stamps, UTIME_NOW picks now, UTIME_OMIT skips one
static int64_t read_utimens(uint64_t u_times, fs::vattr* attr, uint32_t* mask) {
    uint64_t now = clock::realtime_ns();
    if (u_times == 0) {
        attr->atime_ns = now;
        attr->mtime_ns = now;
        *mask = fs::VATTR_ATIME | fs::VATTR_MTIME;
        return 0;
    }

    kernel_timespec ts[2];
    int32_t copy_rc = mm::uaccess::copy_from_user(
        ts, reinterpret_cast<const void*>(u_times), sizeof(ts));
    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    const uint32_t bits[2] = { fs::VATTR_ATIME, fs::VATTR_MTIME };
    uint64_t* stamps[2] = { &attr->atime_ns, &attr->mtime_ns };
    *mask = 0;
    for (int i = 0; i < 2; i++) {
        if (ts[i].tv_nsec == UTIME_OMIT) {
            continue;
        }

        if (ts[i].tv_nsec == UTIME_NOW) {
            *stamps[i] = now;
        } else {
            // Stamps are unsigned nanoseconds since the epoch, so the range
            // stops at 1970 below and at the uint64_t limit above
            bool bad_nsec = ts[i].tv_nsec < 0 || ts[i].tv_nsec >= static_cast<int64_t>(NS_PER_SEC);
            bool bad_sec = ts[i].tv_sec < 0 ||
                static_cast<uint64_t>(ts[i].tv_sec) > ~0ULL / NS_PER_SEC;
            if (bad_nsec || bad_sec) {
                return syscall::EINVAL;
            }
            *stamps[i] = static_cast<uint64_t>(ts[i].tv_sec) * NS_PER_SEC +
                static_cast<uint64_t>(ts[i].tv_nsec);
        }
        *mask |= bits[i];
    }

    return 0;
}

static int64_t set_fd_times(sched::task* task, int64_t fd, const fs::vattr& attr, uint32_t mask) {
    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    if (obj->type != resource::resource_type::FILE) {
        resource::resource_release(obj);
        return syscall::EBADF;
    }

    fs::file* kfile = resource::file_provider::get_file(obj);
    if (!kfile) {
        resource::resource_release(obj);
        return syscall::EIO;
    }

    int32_t fs_rc = fs::fsetattr(kfile, attr, mask);
    resource::resource_release(obj);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    return 0;
}

static int64_t do_newfstatat_common(int64_t dirfd, uint64_t pathname, uint64_t u_stat, uint64_t flags) {
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

        if (dirfd == AT_FDCWD) {
            sched::task* task = sched::current();
            if (!task) {
                return syscall::EIO;
            }

            fs::node* cwd = nullptr;
            int64_t cwd_rc = acquire_task_cwd_node(task, &cwd);
            if (cwd_rc != 0) {
                return cwd_rc;
            }

            fs::vattr attr = {};
            int32_t fs_rc = cwd->getattr(&attr);
            release_node_ref(cwd);
            if (fs_rc != fs::OK) {
                return syscall::error_map::map_fs_error(fs_rc);
            }

            return copy_stat_to_user(attr, u_stat);
        }

        if (dirfd < 0) {
            return syscall::EBADF;
        }

        return do_fstat_common(dirfd, u_stat);
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    fs::node* target = nullptr;
    uint32_t lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? fs::LOOKUP_NOFOLLOW : 0;
    int64_t lookup_rc = lookup_node_for_dirfd_path(task, dirfd, kpath, &target, lookup_flags);
    if (lookup_rc != 0) {
        return lookup_rc;
    }

    fs::vattr attr = {};
    int32_t fs_rc = target->getattr(&attr);
    release_node_ref(target);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    return copy_stat_to_user(attr, u_stat);
}

static int64_t do_open_common(int64_t dirfd, uint64_t pathname, uint64_t flags, uint64_t mode) {
    (void)mode;

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

    if (kpath[0] == '\0') {
        return syscall::ENOENT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    uint32_t open_flags = static_cast<uint32_t>(flags);
    char* resolved_path = static_cast<char*>(heap::uzalloc(fs::PATH_MAX));
    if (!resolved_path) {
        return syscall::ENOMEM;
    }

    const char* path_for_open = nullptr;
    if (kpath[0] == '/') {
        int64_t norm_rc = normalize_absolute_path(
            nullptr, kpath, resolved_path, fs::PATH_MAX);
        if (norm_rc != 0) {
            heap::ufree(resolved_path);
            return norm_rc;
        }

        if (resource::shm_provider::is_shm_path(resolved_path)) {
            path_for_open = resolved_path;
        } else {
            int64_t resolve_rc = resolve_open_resource_path(
                task, dirfd, kpath, open_flags, resolved_path, fs::PATH_MAX);
            if (resolve_rc != 0) {
                heap::ufree(resolved_path);
                return resolve_rc;
            }

            path_for_open = resolved_path;
        }
    } else {
        int64_t norm_rc = normalize_path_for_dirfd(
            task, dirfd, kpath, resolved_path, fs::PATH_MAX);
        if (norm_rc != 0) {
            heap::ufree(resolved_path);
            return norm_rc;
        }

        if (!resource::shm_provider::is_shm_path(resolved_path)) {
            int64_t resolve_rc = resolve_open_resource_path(
                task, dirfd, kpath, open_flags, resolved_path, fs::PATH_MAX);
            if (resolve_rc != 0) {
                heap::ufree(resolved_path);
                return resolve_rc;
            }
        }
        path_for_open = resolved_path;
    }

    resource::handle_t handle = -1;
    int32_t rc = resource::open(
        task,
        path_for_open,
        open_flags,
        &handle
    );
    heap::ufree(resolved_path);
    if (rc != resource::OK) {
        return map_resource_error(rc);
    }

    return handle;
}

DEFINE_SYSCALL4(openat, dirfd, pathname, flags, mode) {
    return do_open_common(static_cast<int64_t>(dirfd), pathname, flags, mode);
}

DEFINE_SYSCALL3(open, pathname, flags, mode) {
    return do_open_common(AT_FDCWD, pathname, flags, mode);
}

DEFINE_SYSCALL3(lseek, fd, offset, whence) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
    if (rc != resource::HANDLE_OK || !obj) {
        return syscall::EBADF;
    }

    if (obj->type != resource::resource_type::FILE) {
        resource::resource_release(obj);
        return syscall::ESPIPE;
    }

    fs::file* f = resource::file_provider::get_file(obj);
    if (!f) {
        resource::resource_release(obj);
        return syscall::EBADF;
    }

    int64_t result = fs::seek(f, static_cast<int64_t>(offset),
                              static_cast<int>(whence));
    resource::resource_release(obj);
    if (result < 0) {
        return syscall::error_map::map_fs_error(static_cast<int32_t>(result));
    }

    return result;
}

// A positional transfer needs a non-negative offset and must end within the largest offset
static bool is_valid_file_range(uint64_t offset, uint64_t count) {
    return static_cast<int64_t>(offset) >= 0 && static_cast<int64_t>(count) >= 0 &&
           static_cast<int64_t>(offset + count) >= 0;
}

__PRIVILEGED_CODE static int64_t read_to_user(
    sched::task* task, uint64_t fd, uint64_t buf, uint64_t count, int64_t offset, uint32_t call_flags = 0
) {
    size_t remaining = static_cast<size_t>(count);
    uint8_t* user_ptr = reinterpret_cast<uint8_t*>(buf);
    int64_t total = 0;

    size_t stage = syscall::io_chunk_size(task, static_cast<resource::handle_t>(fd));
    uint8_t* kbuf = static_cast<uint8_t*>(heap::uzalloc(stage));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    while (remaining > 0) {
        size_t chunk = remaining > stage ? stage : remaining;
        ssize_t n = 0;
        if (offset == CURRENT_FILE_OFFSET) {
            n = resource::read(task, static_cast<resource::handle_t>(fd), kbuf, chunk, call_flags);
        } else {
            n = resource::read_at(task, static_cast<resource::handle_t>(fd), kbuf, chunk,
                                  static_cast<uint64_t>(offset + total));
        }

        if (n < 0) {
            heap::ufree(kbuf);
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
            heap::ufree(kbuf);
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

    heap::ufree(kbuf);
    return total;
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

    return read_to_user(task, fd, buf, count, CURRENT_FILE_OFFSET);
}

DEFINE_SYSCALL4(pread64, fd, buf, count, offset) {
    if (!is_valid_file_range(offset, count)) {
        return syscall::EINVAL;
    }

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

    return read_to_user(task, fd, buf, count, static_cast<int64_t>(offset));
}

__PRIVILEGED_CODE static int64_t write_from_user(
    sched::task* task, uint64_t fd, uint64_t buf, uint64_t count, int64_t offset, uint32_t call_flags = 0
) {
    size_t remaining = static_cast<size_t>(count);
    const uint8_t* user_ptr = reinterpret_cast<const uint8_t*>(buf);
    int64_t total = 0;

    size_t stage = syscall::io_chunk_size(task, static_cast<resource::handle_t>(fd));
    uint8_t* kbuf = static_cast<uint8_t*>(heap::uzalloc(stage));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    while (remaining > 0) {
        size_t chunk = remaining > stage ? stage : remaining;
        int32_t copy_rc = mm::uaccess::copy_from_user(kbuf, user_ptr, chunk);
        if (copy_rc != mm::uaccess::OK) {
            heap::ufree(kbuf);
            if (total > 0) {
                return total;
            }

            return syscall::EFAULT;
        }

        ssize_t n = 0;
        if (offset == CURRENT_FILE_OFFSET) {
            n = resource::write(task, static_cast<resource::handle_t>(fd), kbuf, chunk, call_flags);
        } else {
            n = resource::write_at(task, static_cast<resource::handle_t>(fd), kbuf, chunk,
                                   static_cast<uint64_t>(offset + total));
        }

        if (n < 0) {
            heap::ufree(kbuf);
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

    heap::ufree(kbuf);
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

    return write_from_user(task, fd, buf, count, CURRENT_FILE_OFFSET);
}

DEFINE_SYSCALL4(pwrite64, fd, buf, count, offset) {
    if (!is_valid_file_range(offset, count)) {
        return syscall::EINVAL;
    }

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

    return write_from_user(task, fd, buf, count, static_cast<int64_t>(offset));
}

/**
 * Copies the buffer list of a vectored transfer into memory the caller frees with heap::ufree.
 * The lengths must add up to a total the transfer can report.
 */
__PRIVILEGED_CODE static int64_t copy_iovecs_from_user(
    uint64_t u_iov,
    uint64_t iovcnt,
    syscall::iovec** out_iovs,
    uint64_t* out_total_len
) {
    if (iovcnt > syscall::MAX_IOVCNT) {
        return syscall::EINVAL;
    }

    size_t size = static_cast<size_t>(iovcnt) * sizeof(syscall::iovec);
    auto* iovs = static_cast<syscall::iovec*>(heap::uzalloc(size));
    if (!iovs) {
        return syscall::ENOMEM;
    }

    if (mm::uaccess::copy_from_user(iovs, reinterpret_cast<const void*>(u_iov), size) != mm::uaccess::OK) {
        heap::ufree(iovs);
        return syscall::EFAULT;
    }

    uint64_t total_len = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        bool overflows = __builtin_add_overflow(total_len, iovs[i].len, &total_len);
        if (overflows || static_cast<int64_t>(total_len) < 0) {
            heap::ufree(iovs);
            return syscall::EINVAL;
        }
    }

    *out_iovs = iovs;
    *out_total_len = total_len;

    return 0;
}

__PRIVILEGED_CODE static int64_t transfer_buffers(
    sched::task* task,
    uint64_t fd,
    const syscall::iovec* iovs,
    uint64_t iovcnt,
    int64_t offset,
    uint32_t call_flags,
    staged_transfer_fn transfer
) {
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        int64_t buffer_offset = offset == CURRENT_FILE_OFFSET ? CURRENT_FILE_OFFSET : offset + total;
        int64_t n = transfer(task, fd, iovs[i].base, iovs[i].len, buffer_offset, call_flags);
        if (n < 0) {
            return total > 0 ? total : n;
        }

        total += n;
        if (static_cast<uint64_t>(n) < iovs[i].len) {
            break;
        }
    }

    return total;
}

__PRIVILEGED_CODE static int64_t run_vectored_transfer(
    uint64_t fd,
    uint64_t u_iov,
    uint64_t iovcnt,
    int64_t offset,
    uint32_t call_flags,
    staged_transfer_fn transfer
) {
    if (iovcnt == 0) {
        return 0;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    syscall::iovec* iovs = nullptr;
    uint64_t total_len = 0;
    int64_t rc = copy_iovecs_from_user(u_iov, iovcnt, &iovs, &total_len);
    if (rc != 0) {
        return rc;
    }

    int64_t result = syscall::EINVAL;
    if (offset == CURRENT_FILE_OFFSET || is_valid_file_range(static_cast<uint64_t>(offset), total_len)) {
        result = transfer_buffers(task, fd, iovs, iovcnt, offset, call_flags, transfer);
    }

    heap::ufree(iovs);

    return result;
}

DEFINE_SYSCALL4(preadv, fd, iov, iovcnt, offset) {
    if (static_cast<int64_t>(offset) < 0) {
        return syscall::EINVAL;
    }

    return run_vectored_transfer(fd, iov, iovcnt, static_cast<int64_t>(offset), 0, read_to_user);
}

DEFINE_SYSCALL4(pwritev, fd, iov, iovcnt, offset) {
    if (static_cast<int64_t>(offset) < 0) {
        return syscall::EINVAL;
    }

    return run_vectored_transfer(fd, iov, iovcnt, static_cast<int64_t>(offset), 0, write_from_user);
}

constexpr uint64_t RWF_HIPRI  = 0x01;
constexpr uint64_t RWF_DSYNC  = 0x02;
constexpr uint64_t RWF_SYNC   = 0x04;
constexpr uint64_t RWF_NOWAIT = 0x08;

// Files live in memory, so every transfer is already as durable as the sync flags ask.
// RWF_APPEND is refused, since it must put the whole call at the end of the file at once.
constexpr uint64_t SUPPORTED_RWF_FLAGS = RWF_HIPRI | RWF_DSYNC | RWF_SYNC | RWF_NOWAIT;

__PRIVILEGED_CODE static int64_t run_vectored_transfer_v2(
    uint64_t fd,
    uint64_t u_iov,
    uint64_t iovcnt,
    uint64_t offset,
    uint64_t flags,
    staged_transfer_fn transfer
) {
    if (static_cast<int64_t>(offset) < CURRENT_FILE_OFFSET) {
        return syscall::EINVAL;
    }

    if (flags & ~SUPPORTED_RWF_FLAGS) {
        return syscall::EOPNOTSUPP;
    }

    // Only transfers at the file position can wait, since explicit offsets reach only files in memory
    uint32_t call_flags = (flags & RWF_NOWAIT) ? fs::O_NONBLOCK : 0;

    return run_vectored_transfer(fd, u_iov, iovcnt, static_cast<int64_t>(offset), call_flags, transfer);
}

// 64-bit callers pass the whole offset in `offset`, and `offset_high` exists for 32-bit ones
DEFINE_SYSCALL6(preadv2, fd, iov, iovcnt, offset, offset_high, flags) {
    (void)offset_high;
    return run_vectored_transfer_v2(fd, iov, iovcnt, offset, flags, read_to_user);
}

DEFINE_SYSCALL6(pwritev2, fd, iov, iovcnt, offset, offset_high, flags) {
    (void)offset_high;
    return run_vectored_transfer_v2(fd, iov, iovcnt, offset, flags, write_from_user);
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

DEFINE_SYSCALL2(lstat, pathname, statbuf) {
    return do_newfstatat_common(AT_FDCWD, pathname, statbuf, AT_SYMLINK_NOFOLLOW);
}

DEFINE_SYSCALL2(fstat, fd, statbuf) {
    return do_fstat_common(static_cast<int64_t>(fd), statbuf);
}

DEFINE_SYSCALL4(newfstatat, dirfd, pathname, statbuf, flags) {
    return do_newfstatat_common(
        static_cast<int64_t>(dirfd), pathname, statbuf, flags);
}

DEFINE_SYSCALL2(getcwd, buf, size) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    fs::node* cwd = nullptr;
    int64_t cwd_rc = acquire_task_cwd_node(task, &cwd);
    if (cwd_rc != 0) {
        return cwd_rc;
    }

    char cwd_path[fs::PATH_MAX];
    int32_t path_rc = fs::path_from_node(cwd, cwd_path, sizeof(cwd_path));
    release_node_ref(cwd);
    if (path_rc != fs::OK) {
        return syscall::error_map::map_fs_error(path_rc);
    }

    size_t required = string::strnlen(cwd_path, sizeof(cwd_path)) + 1;
    if (size == 0 || size < required) {
        return syscall::ERANGE;
    }

    if (buf == 0) {
        return syscall::EFAULT;
    }

    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(buf), cwd_path, required);
    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return static_cast<int64_t>(required);
}

DEFINE_SYSCALL1(chdir, pathname) {
    if (pathname == 0) {
        return syscall::EFAULT;
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
        return syscall::ENOENT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    fs::node* base = nullptr;
    int64_t base_rc = acquire_task_cwd_node(task, &base);
    if (base_rc != 0) {
        return base_rc;
    }

    fs::node* target = nullptr;
    int32_t fs_rc = fs::lookup_at(base, kpath, &target);
    release_node_ref(base);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    int64_t set_rc = replace_task_cwd_node(task, target);
    release_node_ref(target);
    return set_rc;
}

DEFINE_SYSCALL1(fchdir, fd) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    int64_t dirfd = static_cast<int64_t>(fd);
    if (dirfd == AT_FDCWD) {
        return syscall::EBADF;
    }

    fs::node* dir = nullptr;
    int64_t dir_rc = resolve_dirfd_base_node(task, dirfd, &dir);
    if (dir_rc != 0) {
        return dir_rc;
    }

    int64_t set_rc = replace_task_cwd_node(task, dir);
    release_node_ref(dir);
    return set_rc;
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
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
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

            return syscall::error_map::map_fs_error(static_cast<int32_t>(nread));
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
        hdr.d_ino = entry.ino;
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

constexpr uint64_t F_DUPFD = 0;
constexpr uint64_t F_GETFD = 1;
constexpr uint64_t F_SETFD = 2;
constexpr uint64_t F_GETFL = 3;
constexpr uint64_t F_SETFL = 4;
constexpr uint64_t F_GETLK = 5;
constexpr uint64_t F_SETLK = 6;
constexpr uint64_t F_SETLKW = 7;
constexpr uint64_t F_DUPFD_CLOEXEC = 1030;

constexpr int16_t F_RDLCK = 0;
constexpr int16_t F_WRLCK = 1;
constexpr int16_t F_UNLCK = 2;

constexpr int64_t FD_CLOEXEC = 1;

/**
 * Resolves the bytes an flock covers. They count from the start, the file offset, or the end,
 * a zero length reaches the end of the file however far it grows, and a negative length covers
 * the bytes before the starting point.
 */
static int64_t resolve_record_lock_range(
    fs::file* f,
    const linux_flock& flock,
    uint64_t* out_start,
    uint64_t* out_end
) {
    int64_t base = 0;
    if (flock.l_whence == fs::SEEK_CUR) {
        base = f->offset();
    } else if (flock.l_whence == fs::SEEK_END) {
        fs::vattr attr = {};
        int32_t rc = fs::fstat(f, &attr);
        if (rc != fs::OK) {
            return syscall::error_map::map_fs_error(rc);
        }

        base = static_cast<int64_t>(attr.size);
    } else if (flock.l_whence != fs::SEEK_SET) {
        return syscall::EINVAL;
    }

    if (flock.l_start > fs::MAX_FILE_OFFSET - base) {
        return syscall::EOVERFLOW;
    }

    int64_t start = base + flock.l_start;
    if (start < 0) {
        return syscall::EINVAL;
    }

    int64_t end = fs::MAX_FILE_OFFSET;
    if (flock.l_len > 0) {
        if (flock.l_len - 1 > fs::MAX_FILE_OFFSET - start) {
            return syscall::EOVERFLOW;
        }

        end = start + (flock.l_len - 1);
    } else if (flock.l_len < 0) {
        if (start + flock.l_len < 0) {
            return syscall::EINVAL;
        }

        end = start - 1;
        start += flock.l_len;
    }

    *out_start = static_cast<uint64_t>(start);
    *out_end = static_cast<uint64_t>(end);

    return 0;
}

static int64_t map_record_lock_error(int32_t fs_result) {
    switch (fs_result) {
        case fs::OK:        return 0;
        case fs::ERR_AGAIN: return syscall::EAGAIN;
        case fs::ERR_INTR:  return syscall::ERESTARTSYS;
        case fs::ERR_NOMEM: return syscall::ENOLCK;
        default:            return syscall::EIO;
    }
}

/**
 * Fills `flock` with the first lock of another owner that would block `request`, or sets only its
 * type to F_UNLCK when none would.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void describe_conflicting_lock(
    fs::file* f,
    const fs::record_lock& request,
    linux_flock& flock
) {
    fs::record_lock conflict = {};
    if (!f->get_node()->record_locks().find_conflict(request, &conflict)) {
        flock.l_type = F_UNLCK;
        return;
    }

    bool reaches_end_of_file = conflict.end == static_cast<uint64_t>(fs::MAX_FILE_OFFSET);
    int64_t length = reaches_end_of_file ? 0 : static_cast<int64_t>(conflict.end - conflict.start + 1);

    flock.l_type = conflict.type == fs::record_lock_type::shared ? F_RDLCK : F_WRLCK;
    flock.l_whence = static_cast<int16_t>(fs::SEEK_SET);
    flock.l_start = static_cast<int64_t>(conflict.start);
    flock.l_len = length;
    flock.l_pid = conflict.pid;
}

/**
 * Takes a shared lock, which needs read access through the handle, or an exclusive lock, which
 * needs write access, waiting for conflicting locks to be released when `wait` is set.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int64_t take_record_lock(
    fs::file* f,
    const fs::record_lock& request,
    uint32_t rights,
    bool wait
) {
    bool is_shared = request.type == fs::record_lock_type::shared;
    uint32_t guarded_access = is_shared ? resource::RIGHT_READ : resource::RIGHT_WRITE;
    if (!(rights & guarded_access)) {
        return syscall::EBADF;
    }

    fs::record_lock_table& locks = f->get_node()->record_locks();
    int32_t result = wait ? locks.lock(request) : locks.try_lock(request);

    return map_record_lock_error(result);
}

/**
 * Runs F_GETLK, F_SETLK, or F_SETLKW with the flock at `u_flock`. The locks belong to the caller's
 * handle table, so closing any of its handles to the file releases them.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int64_t run_record_lock_command(
    sched::task* task,
    resource::resource_object* obj,
    uint32_t rights,
    uint64_t cmd,
    uint64_t u_flock
) {
    linux_flock flock = {};
    int32_t copy_rc = mm::uaccess::copy_from_user(&flock, reinterpret_cast<const void*>(u_flock), sizeof(flock));
    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    // Only files carry record locks
    fs::file* f = obj->type == resource::resource_type::FILE ? resource::file_provider::get_file(obj) : nullptr;
    if (!f) {
        return syscall::EINVAL;
    }

    bool is_lock = flock.l_type == F_RDLCK || flock.l_type == F_WRLCK;
    bool is_unlock = cmd != F_GETLK && flock.l_type == F_UNLCK;
    if (!is_lock && !is_unlock) {
        return syscall::EINVAL;
    }

    fs::record_lock request = {};
    int64_t range_rc = resolve_record_lock_range(f, flock, &request.start, &request.end);
    if (range_rc != 0) {
        return range_rc;
    }

    request.owner = task->handles;
    request.type = flock.l_type == F_WRLCK ? fs::record_lock_type::exclusive : fs::record_lock_type::shared;
    request.pid = static_cast<int32_t>(task->group ? task->group->pid : task->tid);

    if (cmd == F_GETLK) {
        describe_conflicting_lock(f, request, flock);
        copy_rc = mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_flock), &flock, sizeof(flock));
        return copy_rc == mm::uaccess::OK ? 0 : syscall::EFAULT;
    }

    if (is_unlock) {
        fs::record_lock_table& locks = f->get_node()->record_locks();
        return map_record_lock_error(locks.unlock(request.owner, request.start, request.end));
    }

    return take_record_lock(f, request, rights, cmd == F_SETLKW);
}

DEFINE_SYSCALL3(fcntl, fd, cmd, arg) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        return syscall::duplicate_handle(task, static_cast<resource::handle_t>(fd), arg,
                                         cmd == F_DUPFD_CLOEXEC);
    }

    if (cmd == F_GETFD) {
        uint32_t flags = 0;
        int32_t rc = resource::get_handle_flags(
            task->handles, static_cast<resource::handle_t>(fd), &flags);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }

        return (flags & resource::RESOURCE_HANDLE_CLOEXEC) ? FD_CLOEXEC : 0;
    }

    if (cmd == F_SETFD) {
        uint32_t flags = 0;
        int32_t rc = resource::get_handle_flags(
            task->handles, static_cast<resource::handle_t>(fd), &flags);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }

        flags &= ~resource::RESOURCE_HANDLE_CLOEXEC;
        if (arg & FD_CLOEXEC) {
            flags |= resource::RESOURCE_HANDLE_CLOEXEC;
        }

        rc = resource::set_handle_flags(
            task->handles, static_cast<resource::handle_t>(fd), flags);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }

        return 0;
    }

    if (cmd == F_GETFL) {
        resource::resource_object* obj = nullptr;
        uint32_t rights = 0;
        int32_t rc = resource::get_handle_object(
            task->handles, static_cast<resource::handle_t>(fd), 0,
            &obj, nullptr, &rights);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }

        uint32_t status = resource::get_status_flags(obj);
        resource::resource_release(obj);

        // The access mode lives in the handle rights rather than the
        // flag word, and callers use it to judge fd writability
        uint32_t accmode = fs::O_RDONLY;
        if ((rights & resource::RIGHT_READ) && (rights & resource::RIGHT_WRITE)) {
            accmode = fs::O_RDWR;
        } else if (rights & resource::RIGHT_WRITE) {
            accmode = fs::O_WRONLY;
        }

        return static_cast<int64_t>(accmode | status);
    }

    if (cmd == F_SETFL) {
        resource::resource_object* obj = nullptr;
        int32_t rc = resource::get_handle_object(
            task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }

        resource::set_status_flags(obj, static_cast<uint32_t>(arg));
        resource::resource_release(obj);

        return 0;
    }

    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
        resource::resource_object* obj = nullptr;
        uint32_t rights = 0;
        int32_t rc = resource::get_handle_object(
            task->handles, static_cast<resource::handle_t>(fd), 0,
            &obj, nullptr, &rights);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }

        int64_t result = run_record_lock_command(task, obj, rights, cmd, arg);
        resource::resource_release(obj);

        return result;
    }

    return syscall::EINVAL;
}

DEFINE_SYSCALL3(unlinkat, dirfd, pathname, flags_val) {
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

    if (kpath[0] == '\0') {
        return syscall::ENOENT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    char* normalized_path = nullptr;
    const char* shm_path = nullptr;
    if (kpath[0] == '/') {
        normalized_path = static_cast<char*>(heap::uzalloc(fs::PATH_MAX));
        if (!normalized_path) {
            return syscall::ENOMEM;
        }

        int64_t norm_rc = normalize_absolute_path(
            nullptr, kpath, normalized_path, fs::PATH_MAX);
        if (norm_rc != 0) {
            heap::ufree(normalized_path);
            return norm_rc;
        }

        if (resource::shm_provider::is_shm_path(normalized_path)) {
            shm_path = normalized_path;
        }
    } else {
        normalized_path = static_cast<char*>(heap::uzalloc(fs::PATH_MAX));
        if (!normalized_path) {
            return syscall::ENOMEM;
        }

        int64_t norm_rc = normalize_path_for_dirfd(
            task, static_cast<int64_t>(dirfd), kpath,
            normalized_path, fs::PATH_MAX);
        if (norm_rc != 0) {
            heap::ufree(normalized_path);
            return norm_rc;
        }

        if (resource::shm_provider::is_shm_path(normalized_path)) {
            shm_path = normalized_path;
        }
    }

    if (shm_path) {
        int32_t rc = resource::shm_provider::unlink_shm(shm_path);
        if (normalized_path) {
            heap::ufree(normalized_path);
        }
        if (rc != resource::OK) {
            return map_resource_error(rc);
        }

        return 0;
    }

    if (normalized_path) {
        heap::ufree(normalized_path);
    }

    fs::node* parent = nullptr;
    const char* name = nullptr;
    size_t name_len = 0;
    int64_t parent_rc = resolve_parent_for_dirfd_path(
        task, static_cast<int64_t>(dirfd), kpath,
        &parent, &name, &name_len);
    if (parent_rc != 0) {
        return parent_rc;
    }

    int32_t rc;
    if (flags_val & AT_REMOVEDIR) {
        rc = parent->rmdir(name, name_len);
    } else {
        rc = parent->unlink(name, name_len);
    }
    release_node_ref(parent);
    if (rc != fs::OK) {
        return syscall::error_map::map_fs_error(rc);
    }

    return 0;
}

DEFINE_SYSCALL1(unlink, pathname) {
    // -100 is AT_FDCWD
    return sys_unlinkat(
        static_cast<uint64_t>(-100),
        pathname, 0, 0, 0, 0);
}

DEFINE_SYSCALL1(rmdir, pathname) {
    // -100 is AT_FDCWD
    return sys_unlinkat(
        static_cast<uint64_t>(-100),
        pathname, AT_REMOVEDIR, 0, 0, 0);
}

DEFINE_SYSCALL3(mkdirat, dirfd, pathname, mode) {
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

    if (kpath[0] == '\0') {
        return syscall::ENOENT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    fs::node* parent = nullptr;
    const char* name = nullptr;
    size_t name_len = 0;
    int64_t parent_rc = resolve_parent_for_dirfd_path(
        task, static_cast<int64_t>(dirfd), kpath,
        &parent, &name, &name_len);
    if (parent_rc != 0) {
        return parent_rc;
    }

    fs::node* child = nullptr;
    int32_t rc = parent->mkdir(name, name_len, static_cast<uint32_t>(mode), &child);
    if (child) {
        if (child->release()) {
            fs::node::ref_destroy(child);
        }
    }
    release_node_ref(parent);
    if (rc != fs::OK) {
        return syscall::error_map::map_fs_error(rc);
    }

    return 0;
}

DEFINE_SYSCALL2(mkdir, pathname, mode) {
    // -100 is AT_FDCWD
    return sys_mkdirat(
        static_cast<uint64_t>(-100),
        pathname, mode, 0, 0, 0);
}

DEFINE_SYSCALL3(faccessat, dirfd, pathname, mode) {
    // F_OK is zero and R_OK, W_OK, X_OK occupy the low three bits
    if (mode & ~7ULL) {
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

    if (kpath[0] == '\0') {
        return syscall::ENOENT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    fs::node* node = nullptr;
    int64_t lookup_rc = lookup_node_for_dirfd_path(
        task, static_cast<int64_t>(dirfd), kpath, &node);
    if (lookup_rc != 0) {
        return lookup_rc;
    }

    // No permission model exists, so any resolvable node satisfies every mode
    release_node_ref(node);
    return 0;
}

DEFINE_SYSCALL3(fchmodat, dirfd, pathname, mode) {
    (void)mode;

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

    if (kpath[0] == '\0') {
        return syscall::ENOENT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    fs::node* node = nullptr;
    int64_t lookup_rc = lookup_node_for_dirfd_path(
        task, static_cast<int64_t>(dirfd), kpath, &node);
    if (lookup_rc != 0) {
        return lookup_rc;
    }

    // The vfs synthesizes permissions from node type and stores no mode
    // bits, so a change on a resolvable node succeeds without effect
    release_node_ref(node);
    return 0;
}

DEFINE_SYSCALL2(chmod, pathname, mode) {
    // -100 is AT_FDCWD
    return sys_fchmodat(
        static_cast<uint64_t>(-100),
        pathname, mode, 0, 0, 0);
}

DEFINE_SYSCALL4(utimensat, dirfd, pathname, times, flags) {
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
        return syscall::EINVAL;
    }

    fs::vattr attr = {};
    uint32_t mask = 0;
    int64_t times_rc = read_utimens(times, &attr, &mask);
    if (times_rc != 0) {
        return times_rc;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    // A null pathname is how futimens arrives: the stamps go on dirfd itself
    if (pathname == 0) {
        if (flags != 0) {
            return syscall::EINVAL;
        }

        if (static_cast<int64_t>(dirfd) == AT_FDCWD) {
            return syscall::EFAULT;
        }

        return set_fd_times(task, static_cast<int64_t>(dirfd), attr, mask);
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
        return syscall::ENOENT;
    }

    fs::node* target = nullptr;
    uint32_t lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? fs::LOOKUP_NOFOLLOW : 0;
    int64_t lookup_rc = lookup_node_for_dirfd_path(
        task, static_cast<int64_t>(dirfd), kpath, &target, lookup_flags);
    if (lookup_rc != 0) {
        return lookup_rc;
    }

    int32_t fs_rc = target->setattr(attr, mask);
    release_node_ref(target);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    return 0;
}

DEFINE_SYSCALL2(access, pathname, mode) {
    // -100 is AT_FDCWD
    return sys_faccessat(
        static_cast<uint64_t>(-100),
        pathname, mode, 0, 0, 0);
}

static int64_t do_readlinkat(int64_t dirfd, uint64_t pathname,
                             uint64_t u_buf, uint64_t bufsize) {
    if (bufsize == 0) {
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

    if (kpath[0] == '\0') {
        return syscall::ENOENT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    fs::node* node = nullptr;
    int64_t lookup_rc = lookup_node_for_dirfd_path(
        task, dirfd, kpath, &node, fs::LOOKUP_NOFOLLOW);
    if (lookup_rc != 0) {
        return lookup_rc;
    }

    // Only symbolic links have a target to read
    if (node->type() != fs::node_type::symlink) {
        release_node_ref(node);
        return syscall::EINVAL;
    }

    // The input path is no longer needed, so reuse kpath for the target
    size_t cap = bufsize < sizeof(kpath) ? bufsize : sizeof(kpath);
    size_t target_len = 0;
    int32_t fs_rc = node->readlink(kpath, cap, &target_len);
    release_node_ref(node);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    // Guard against a provider reporting more than it wrote
    if (target_len > cap) {
        target_len = cap;
    }

    if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_buf),
                                  kpath, target_len) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return static_cast<int64_t>(target_len);
}

DEFINE_SYSCALL4(readlinkat, dirfd, pathname, buf, bufsize) {
    return do_readlinkat(static_cast<int64_t>(dirfd), pathname, buf, bufsize);
}

DEFINE_SYSCALL3(readlink, pathname, buf, bufsize) {
    return do_readlinkat(static_cast<int64_t>(-100), pathname, buf, bufsize);
}

static int64_t copy_user_path(uint64_t u_path, char* out, size_t cap) {
    int32_t copy_rc = mm::uaccess::copy_cstr_from_user(
        out, cap, reinterpret_cast<const char*>(u_path));
    if (copy_rc == mm::uaccess::ERR_NAMETOOLONG) {
        return syscall::ENAMETOOLONG;
    }

    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    if (out[0] == '\0') {
        return syscall::ENOENT;
    }

    return 0;
}

DEFINE_SYSCALL4(renameat, olddirfd, oldpath, newdirfd, newpath) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    // Both paths outlive the rename because parent resolution hands back
    // name pointers into them
    char* paths = static_cast<char*>(heap::uzalloc(2 * fs::PATH_MAX));
    if (!paths) {
        return syscall::ENOMEM;
    }

    char* old_kpath = paths;
    char* new_kpath = paths + fs::PATH_MAX;
    int64_t rc = copy_user_path(oldpath, old_kpath, fs::PATH_MAX);
    if (rc == 0) {
        rc = copy_user_path(newpath, new_kpath, fs::PATH_MAX);
    }
    if (rc != 0) {
        heap::ufree(paths);
        return rc;
    }

    fs::node* old_parent = nullptr;
    const char* old_name = nullptr;
    size_t old_len = 0;
    rc = resolve_parent_for_dirfd_path(
        task, static_cast<int64_t>(olddirfd), old_kpath,
        &old_parent, &old_name, &old_len);
    if (rc != 0) {
        heap::ufree(paths);
        return rc;
    }

    fs::node* new_parent = nullptr;
    const char* new_name = nullptr;
    size_t new_len = 0;
    rc = resolve_parent_for_dirfd_path(
        task, static_cast<int64_t>(newdirfd), new_kpath,
        &new_parent, &new_name, &new_len);
    if (rc != 0) {
        release_node_ref(old_parent);
        heap::ufree(paths);
        return rc;
    }

    int32_t fs_rc = old_parent->rename(old_name, old_len, new_parent, new_name, new_len);
    release_node_ref(new_parent);
    release_node_ref(old_parent);
    heap::ufree(paths);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    return 0;
}

DEFINE_SYSCALL2(rename, oldpath, newpath) {
    // -100 is AT_FDCWD
    return sys_renameat(
        static_cast<uint64_t>(-100), oldpath,
        static_cast<uint64_t>(-100), newpath, 0, 0);
}

DEFINE_SYSCALL3(symlinkat, target, newdirfd, linkpath) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    char* paths = static_cast<char*>(heap::uzalloc(2 * fs::PATH_MAX));
    if (!paths) {
        return syscall::ENOMEM;
    }

    char* ktarget = paths;
    char* klink = paths + fs::PATH_MAX;
    int64_t rc = copy_user_path(target, ktarget, fs::PATH_MAX);
    if (rc == 0) {
        rc = copy_user_path(linkpath, klink, fs::PATH_MAX);
    }
    if (rc != 0) {
        heap::ufree(paths);
        return rc;
    }

    fs::node* parent = nullptr;
    const char* name = nullptr;
    size_t name_len = 0;
    rc = resolve_parent_for_dirfd_path(
        task, static_cast<int64_t>(newdirfd), klink, &parent, &name, &name_len);
    if (rc != 0) {
        heap::ufree(paths);
        return rc;
    }

    fs::node* link = nullptr;
    int32_t fs_rc = parent->symlink(name, name_len, ktarget, &link);
    release_node_ref(link);
    release_node_ref(parent);
    heap::ufree(paths);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    return 0;
}

DEFINE_SYSCALL2(symlink, target, linkpath) {
    // -100 is AT_FDCWD
    return sys_symlinkat(target, static_cast<uint64_t>(-100), linkpath, 0, 0, 0);
}

// fsync is a no-op on ramfs, data is always in memory
DEFINE_SYSCALL1(fsync, fd) {
    (void)fd;
    return 0;
}
