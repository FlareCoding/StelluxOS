#include "syscall/handlers/sys_fd.h"

#include "resource/resource.h"
#include "resource/providers/file_provider.h"
#include "resource/providers/shmem_resource_provider.h"
#include "resource/providers/shm_provider.h"
#include "syscall/handlers/sys_error_map.h"
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
            return 0;
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

inline void release_node_ref(fs::node* n) {
    if (!n) {
        return;
    }
    if (n->release()) {
        fs::node::ref_destroy(n);
    }
}

int64_t acquire_task_cwd_node(sched::task* task, fs::node** out_node) {
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

int64_t replace_task_cwd_node(sched::task* task, fs::node* new_cwd) {
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

int64_t resolve_dirfd_base_node(
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
        &task->handles, static_cast<resource::handle_t>(dirfd), 0, &obj);
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

int64_t normalize_absolute_path(
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

int64_t normalize_path_for_dirfd(
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

    char* base_path = static_cast<char*>(heap::kzalloc(fs::PATH_MAX));
    if (!base_path) {
        release_node_ref(base_node);
        return syscall::ENOMEM;
    }

    int32_t base_path_rc = fs::path_from_node(base_node, base_path, fs::PATH_MAX);
    release_node_ref(base_node);
    if (base_path_rc != fs::OK) {
        heap::kfree(base_path);
        return syscall::error_map::map_fs_error(base_path_rc);
    }

    int64_t norm_rc = normalize_absolute_path(base_path, input_path, out_path, out_cap);
    heap::kfree(base_path);
    return norm_rc;
}

int64_t lookup_node_for_dirfd_path(
    sched::task* task,
    int64_t dirfd,
    const char* input_path,
    fs::node** out_node
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

    int32_t fs_rc = fs::lookup_at(base, input_path, out_node);
    release_node_ref(base);
    if (fs_rc != fs::OK) {
        return syscall::error_map::map_fs_error(fs_rc);
    }

    return 0;
}

int64_t resolve_parent_for_dirfd_path(
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

int64_t resolve_open_resource_path(
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
            return syscall::error_map::map_fs_error(fs_rc);
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
    int64_t lookup_rc = lookup_node_for_dirfd_path(task, dirfd, kpath, &target);
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

int64_t do_open_common(int64_t dirfd, uint64_t pathname, uint64_t flags, uint64_t mode) {
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
    char* resolved_path = static_cast<char*>(heap::kzalloc(fs::PATH_MAX));
    if (!resolved_path) {
        return syscall::ENOMEM;
    }

    const char* path_for_open = nullptr;
    if (kpath[0] == '/') {
        if (resource::shm_provider::is_shm_path(kpath)) {
            int64_t norm_rc = normalize_absolute_path(
                nullptr, kpath, resolved_path, fs::PATH_MAX);
            if (norm_rc != 0) {
                heap::kfree(resolved_path);
                return norm_rc;
            }
            path_for_open = resolved_path;
        } else {
            int64_t resolve_rc = resolve_open_resource_path(
                task, dirfd, kpath, open_flags, resolved_path, fs::PATH_MAX);
            if (resolve_rc != 0) {
                heap::kfree(resolved_path);
                return resolve_rc;
            }
            path_for_open = resolved_path;
        }
    } else {
        int64_t norm_rc = normalize_path_for_dirfd(
            task, dirfd, kpath, resolved_path, fs::PATH_MAX);
        if (norm_rc != 0) {
            heap::kfree(resolved_path);
            return norm_rc;
        }

        if (!resource::shm_provider::is_shm_path(resolved_path)) {
            int64_t resolve_rc = resolve_open_resource_path(
                task, dirfd, kpath, open_flags, resolved_path, fs::PATH_MAX);
            if (resolve_rc != 0) {
                heap::kfree(resolved_path);
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
    heap::kfree(resolved_path);
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
        &task->handles, static_cast<resource::handle_t>(fd),
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
        if (resource::shm_provider::is_shm_path(kpath)) {
            normalized_path = static_cast<char*>(heap::kzalloc(fs::PATH_MAX));
            if (!normalized_path) {
                return syscall::ENOMEM;
            }

            int64_t norm_rc = normalize_absolute_path(
                nullptr, kpath, normalized_path, fs::PATH_MAX);
            if (norm_rc != 0) {
                heap::kfree(normalized_path);
                return norm_rc;
            }
            shm_path = normalized_path;
        }
    } else {
        normalized_path = static_cast<char*>(heap::kzalloc(fs::PATH_MAX));
        if (!normalized_path) {
            return syscall::ENOMEM;
        }

        int64_t norm_rc = normalize_path_for_dirfd(
            task, static_cast<int64_t>(dirfd), kpath,
            normalized_path, fs::PATH_MAX);
        if (norm_rc != 0) {
            heap::kfree(normalized_path);
            return norm_rc;
        }

        if (resource::shm_provider::is_shm_path(normalized_path)) {
            shm_path = normalized_path;
        }
    }

    if (shm_path) {
        int32_t rc = resource::shm_provider::unlink_shm(shm_path);
        if (normalized_path) {
            heap::kfree(normalized_path);
        }
        if (rc != resource::OK) {
            return map_resource_error(rc);
        }
        return 0;
    }

    if (normalized_path) {
        heap::kfree(normalized_path);
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

    int32_t rc = parent->unlink(name, name_len);
    release_node_ref(parent);
    if (rc != fs::OK) {
        return syscall::error_map::map_fs_error(rc);
    }
    return 0;
}
