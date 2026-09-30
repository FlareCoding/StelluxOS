#include "fs/ramfs/ramfs.h"
#include "fs/socket_node.h"
#include "fs/fs.h"
#include "common/string.h"
#include "mm/heap.h"
#include "mm/mm.h"

namespace ramfs {

__PRIVILEGED_CODE static int32_t ramfs_mount_fn(
    fs::driver* drv, const char* source, uint32_t flags,
    void* data, fs::instance** out
) {
    (void)drv; (void)source; (void)flags; (void)data;

    void* root_mem = heap::kzalloc(sizeof(dir_node));
    if (!root_mem) return fs::ERR_NOMEM;

    auto* root = new (root_mem) dir_node(nullptr, "");

    void* inst_mem = heap::kzalloc(sizeof(fs::instance));
    if (!inst_mem) {
        root->~dir_node();
        heap::kfree(root);
        return fs::ERR_NOMEM;
    }

    auto* inst = new (inst_mem) fs::instance(drv, root);

    root->set_filesystem(inst);
    root->set_parent(root);

    *out = inst;
    return fs::OK;
}

__PRIVILEGED_DATA static fs::driver g_ramfs_driver = {
    "ramfs",
    ramfs_mount_fn,
    {}
};

__PRIVILEGED_CODE int32_t init() {
    return fs::register_driver(&g_ramfs_driver);
}

} // namespace ramfs

// Called from fs::init() to register the ramfs driver
extern "C" __PRIVILEGED_CODE int32_t ramfs_init_driver() {
    return ramfs::init();
}

namespace ramfs {

dir_node::dir_node(fs::instance* fs, const char* name)
    : fs::dir_node(fs, name) {
}

int32_t dir_node::create(const char* name, size_t len, uint32_t mode, fs::node** out) {
    if (!name || !out || len == 0) return fs::ERR_INVAL;

    if (len > fs::NAME_MAX) return fs::ERR_NAMETOOLONG;

    (void)mode;

    sync::irq_lock_guard guard(m_lock);

    if (detached()) {
        return fs::ERR_NOENT;
    }

    if (find_child(name, len)) {
        return fs::ERR_EXIST;
    }

    char name_buf[fs::NAME_MAX + 1];
    string::memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    void* mem = heap::kzalloc(sizeof(file_node));
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* child = new (mem) file_node(m_fs, name_buf);
    int32_t rc = child->init();
    if (rc != fs::OK) {
        fs::node::ref_destroy(child);
        return rc;
    }

    attach_child(child);

    *out = child;
    return fs::OK;
}

int32_t dir_node::create_socket(const char* name, size_t len, void* impl, fs::node** out) {
    (void)impl;
    if (!name || !out || len == 0) return fs::ERR_INVAL;

    if (len > fs::NAME_MAX) return fs::ERR_NAMETOOLONG;

    sync::irq_lock_guard guard(m_lock);

    if (detached()) {
        return fs::ERR_NOENT;
    }

    if (find_child(name, len)) {
        return fs::ERR_EXIST;
    }

    char name_buf[fs::NAME_MAX + 1];
    string::memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    void* mem = heap::kzalloc(sizeof(fs::socket_node));
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* child = new (mem) fs::socket_node(m_fs, name_buf);
    attach_child(child);

    *out = child;
    return fs::OK;
}

int32_t dir_node::mkdir(const char* name, size_t len, uint32_t mode, fs::node** out) {
    if (!name || !out || len == 0) return fs::ERR_INVAL;

    if (len > fs::NAME_MAX) return fs::ERR_NAMETOOLONG;

    (void)mode;

    sync::irq_lock_guard guard(m_lock);

    if (detached()) {
        return fs::ERR_NOENT;
    }

    if (find_child(name, len)) {
        return fs::ERR_EXIST;
    }

    char name_buf[fs::NAME_MAX + 1];
    string::memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    void* mem = heap::kzalloc(sizeof(dir_node));
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* child = new (mem) dir_node(m_fs, name_buf);
    attach_child(child);

    *out = child;
    return fs::OK;
}

int32_t dir_node::unlink(const char* name, size_t len) {
    if (!name || len == 0) return fs::ERR_INVAL;

    sync::irq_lock_guard guard(m_lock);

    fs::node* child = find_child(name, len);
    if (!child) {
        return fs::ERR_NOENT;
    }

    if (child->type() == fs::node_type::directory) {
        return fs::ERR_ISDIR;
    }

    detach_child(child);
    return fs::OK;
}

int32_t dir_node::rmdir(const char* name, size_t len) {
    return rmdir_child(name, len);
}

int32_t dir_node::rename(const char* name, size_t len, fs::node* new_parent,
                         const char* new_name, size_t new_len) {
    return rename_child(name, len, new_parent, new_name, new_len);
}

int32_t dir_node::symlink(const char* name, size_t len, const char* target, fs::node** out) {
    if (!name || !out || !target || len == 0) {
        return fs::ERR_INVAL;
    }

    if (len > fs::NAME_MAX) {
        return fs::ERR_NAMETOOLONG;
    }

    sync::irq_lock_guard guard(m_lock);

    if (detached()) {
        return fs::ERR_NOENT;
    }

    if (find_child(name, len)) {
        return fs::ERR_EXIST;
    }

    char name_buf[fs::NAME_MAX + 1];
    string::memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    void* mem = heap::kzalloc(sizeof(symlink_node));
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* child = new (mem) symlink_node(m_fs, name_buf);
    int32_t rc = child->set_target(target);
    if (rc != fs::OK) {
        fs::node::ref_destroy(child);
        return rc;
    }

    attach_child(child);

    *out = child;
    return fs::OK;
}

symlink_node::symlink_node(fs::instance* fs, const char* name)
    : fs::node(fs::node_type::symlink, fs, name)
    , m_target(nullptr)
    , m_target_len(0) {
}

symlink_node::~symlink_node() {
    if (m_target) {
        heap::kfree(m_target);
    }
}

int32_t symlink_node::set_target(const char* target) {
    size_t len = string::strnlen(target, fs::PATH_MAX);
    if (len == 0 || len >= fs::PATH_MAX) {
        return fs::ERR_INVAL;
    }

    auto* copy = static_cast<char*>(heap::kzalloc(len + 1));
    if (!copy) {
        return fs::ERR_NOMEM;
    }

    string::memcpy(copy, target, len);
    m_target = copy;
    m_target_len = len;
    m_size = len;

    return fs::OK;
}

int32_t symlink_node::readlink(char* buf, size_t size, size_t* out_len) {
    if (!buf || !out_len) {
        return fs::ERR_INVAL;
    }

    size_t n = m_target_len < size ? m_target_len : size;
    string::memcpy(buf, m_target, n);
    *out_len = n;

    return fs::OK;
}

static int32_t map_shmem_error_to_fs(int32_t shmem_err) {
    return shmem_err == mm::SHMEM_ERR_NO_MEM ? fs::ERR_NOMEM : fs::ERR_INVAL;
}

file_node::file_node(fs::instance* fs, const char* name)
    : fs::node(fs::node_type::regular, fs, name) {
}

int32_t file_node::init() {
    mm::shmem* backing = mm::shmem_create(0);
    if (!backing) {
        return fs::ERR_NOMEM;
    }

    m_backing = rc::strong_ref<mm::shmem>::adopt(backing);
    return fs::OK;
}

// stat reads the node's size, so it mirrors the backing's
void file_node::update_size_from_backing_locked() {
    m_size = m_backing->m_size;
}

ssize_t file_node::write_at_locked(size_t offset, const void* buf, size_t count) {
    if (count == 0) {
        return 0;
    }

    size_t end = offset + count;
    if (end < offset) {
        return fs::ERR_INVAL;
    }

    if (end > m_backing->m_size) {
        int32_t rc = mm::shmem_resize_locked(m_backing.ptr(), end);
        if (rc != mm::SHMEM_OK) {
            return map_shmem_error_to_fs(rc);
        }

        update_size_from_backing_locked();
    }

    ssize_t written = mm::shmem_write_locked(m_backing.ptr(), offset, buf, count);
    mark_modified();

    return written;
}

ssize_t file_node::read(fs::file* f, void* buf, size_t count, uint32_t) {
    if (!f || !buf) {
        return fs::ERR_BADF;
    }

    sync::mutex_lock(m_backing->lock);

    int64_t offset = f->offset();
    ssize_t result = fs::ERR_INVAL;
    if (offset >= 0) {
        result = mm::shmem_read_locked(m_backing.ptr(), static_cast<size_t>(offset), buf, count);
    }

    if (result > 0) {
        f->set_offset(offset + result);
    }

    sync::mutex_unlock(m_backing->lock);

    return result;
}

ssize_t file_node::write(fs::file* f, const void* buf, size_t count, uint32_t flags) {
    if (!f || !buf) {
        return fs::ERR_BADF;
    }

    sync::mutex_lock(m_backing->lock);

    int64_t offset = f->offset();
    if (flags & fs::O_APPEND) {
        offset = static_cast<int64_t>(m_backing->m_size);
    }

    ssize_t result = fs::ERR_INVAL;
    if (offset >= 0) {
        result = write_at_locked(static_cast<size_t>(offset), buf, count);
    }

    if (result > 0) {
        f->set_offset(offset + result);
    }

    sync::mutex_unlock(m_backing->lock);

    return result;
}

int64_t file_node::seek(fs::file* f, int64_t offset, int whence) {
    if (!f) return fs::ERR_BADF;

    sync::mutex_lock(m_backing->lock);

    int64_t new_off = fs::ERR_INVAL;
    switch (whence) {
        case fs::SEEK_SET:
            new_off = offset;
            break;
        case fs::SEEK_CUR:
            new_off = f->offset() + offset;
            break;
        case fs::SEEK_END:
            new_off = static_cast<int64_t>(m_backing->m_size) + offset;
            break;
        default:
            break;
    }

    if (new_off >= 0) {
        f->set_offset(new_off);
    } else {
        new_off = fs::ERR_INVAL;
    }

    sync::mutex_unlock(m_backing->lock);

    return new_off;
}

ssize_t file_node::read_at(fs::file*, void* buf, size_t count, uint64_t offset) {
    sync::mutex_lock(m_backing->lock);

    ssize_t result = mm::shmem_read_locked(m_backing.ptr(), static_cast<size_t>(offset), buf, count);

    sync::mutex_unlock(m_backing->lock);

    return result;
}

ssize_t file_node::write_at(fs::file*, const void* buf, size_t count, uint64_t offset) {
    sync::mutex_lock(m_backing->lock);

    ssize_t result = write_at_locked(static_cast<size_t>(offset), buf, count);

    sync::mutex_unlock(m_backing->lock);

    return result;
}

int32_t file_node::truncate(size_t size) {
    sync::mutex_lock(m_backing->lock);

    int32_t rc = mm::shmem_resize_locked(m_backing.ptr(), size);
    if (rc == mm::SHMEM_OK) {
        update_size_from_backing_locked();
        mark_modified();
    }

    sync::mutex_unlock(m_backing->lock);

    return rc == mm::SHMEM_OK ? fs::OK : map_shmem_error_to_fs(rc);
}

int32_t file_node::allocate(uint64_t offset, uint64_t length) {
    uint64_t end = offset + length;
    if (end < offset) {
        return fs::ERR_INVAL;
    }

    sync::mutex_lock(m_backing->lock);

    int32_t rc = mm::shmem_grow_locked(m_backing.ptr(), end);
    if (rc == mm::SHMEM_OK) {
        update_size_from_backing_locked();
        mark_modified();
    }

    sync::mutex_unlock(m_backing->lock);

    return rc == mm::SHMEM_OK ? fs::OK : map_shmem_error_to_fs(rc);
}

int32_t file_node::mmap(fs::file*, mm::mm_context* mm_ctx, uintptr_t addr,
                        size_t length, uint32_t prot, uint32_t map_flags,
                        uint64_t offset, uintptr_t* out_addr) {
    return mm::mm_context_map_shared(mm_ctx, m_backing.ptr(), offset, length, prot,
                                     map_flags, addr, out_addr);
}

} // namespace ramfs
