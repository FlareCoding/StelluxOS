#include "fs/fs.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/mount.h"
#include "fs/path.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"
#include "fs/cpio/cpio.h"
#include "boot/boot_services.h"

extern "C" __PRIVILEGED_CODE int32_t ramfs_init_driver();
extern "C" __PRIVILEGED_CODE int32_t devfs_init_driver();

namespace fs {


__PRIVILEGED_BSS static mount_point* g_root_mount;
__PRIVILEGED_BSS static instance*    g_root_instance;

__PRIVILEGED_DATA static sync::spinlock g_mount_lock = sync::SPINLOCK_INIT;

__PRIVILEGED_BSS static uint32_t g_driver_count;
static constexpr uint32_t MAX_DRIVERS = 16;
__PRIVILEGED_BSS static driver* g_drivers[MAX_DRIVERS];

__PRIVILEGED_BSS static uint32_t g_mount_count;
static constexpr uint32_t MAX_MOUNTS = 32;
__PRIVILEGED_BSS static mount_point* g_mounts[MAX_MOUNTS];


node::node(node_type t, instance* fs, const char* name)
    : m_child_link{}
    , m_type(t)
    , m_fs(fs)
    , m_parent(nullptr)
    , m_size(0)
    , m_lock(sync::SPINLOCK_INIT)
    , m_mounted_here(nullptr) {
    if (name) {
        size_t len = string::strnlen(name, NAME_MAX);
        string::memcpy(m_name, name, len);
        m_name[len] = '\0';
    } else {
        m_name[0] = '\0';
    }
}

__PRIVILEGED_CODE int32_t node::lookup(const char*, size_t, node**)   { return ERR_NOSYS; }
int32_t node::create(const char*, size_t, uint32_t, node**) { return ERR_NOSYS; }
int32_t node::mkdir(const char*, size_t, uint32_t, node**)  { return ERR_NOSYS; }
int32_t node::unlink(const char*, size_t)           { return ERR_NOSYS; }
int32_t node::rmdir(const char*, size_t)            { return ERR_NOSYS; }
ssize_t node::read(file*, void*, size_t)            { return ERR_NOSYS; }
ssize_t node::write(file*, const void*, size_t)     { return ERR_NOSYS; }
int64_t node::seek(file*, int64_t, int)             { return ERR_NOSYS; }
ssize_t node::readdir(file*, dirent*, size_t)       { return ERR_NOSYS; }
int32_t node::ioctl(file*, uint32_t, uint64_t)      { return ERR_NOSYS; }
int32_t node::mmap(file*, mm::mm_context*, uintptr_t, size_t, uint32_t, uint32_t, uint64_t, uintptr_t*) { return ERR_NOSYS; }
int32_t node::open(file*, uint32_t)                 { return OK; }
int32_t node::on_close(file*)                       { return OK; }
int32_t node::readlink(char*, size_t, size_t*)      { return ERR_NOSYS; }
int32_t node::create_socket(const char*, size_t, void*, node**) { return ERR_NOSYS; }
int32_t node::truncate(size_t)                      { return ERR_NOSYS; }

int32_t node::getattr(vattr* attr) {
    if (!attr) return ERR_INVAL;
    attr->type = m_type;
    attr->size = m_size;
    return OK;
}

__PRIVILEGED_CODE void node::ref_destroy(node* n) {
    n->~node();
    heap::kfree(n);
}


file::file(rc::strong_ref<node>&& n, uint32_t flags)
    : m_node(static_cast<rc::strong_ref<node>&&>(n))
    , m_offset(0)
    , m_flags(flags)
    , m_private(nullptr)
    , m_opened(false) {
}

void file::ref_destroy(file* f) {
    RUN_ELEVATED({
        if (f->m_node && f->m_opened) {
            f->m_node->on_close(f);
        }
        f->~file();
        heap::ufree(f);
    });
}


instance::instance(driver* drv, node* root)
    : m_driver(drv)
    , m_root(rc::strong_ref<node>::adopt(root)) {
}

__PRIVILEGED_CODE int32_t instance::unmount() {
    return OK;
}


__PRIVILEGED_CODE int32_t register_driver(driver* drv) {
    if (!drv || !drv->name || !drv->mount_fn) return ERR_INVAL;

    sync::irq_lock_guard guard(g_mount_lock);
    if (g_driver_count >= MAX_DRIVERS) {
        return ERR_NOMEM;
    }
    g_drivers[g_driver_count++] = drv;
    log::info("fs: registered driver '%s'", drv->name);
    return OK;
}

__PRIVILEGED_CODE static driver* find_driver(const char* name) {
    for (uint32_t i = 0; i < g_driver_count; i++) {
        if (string::strcmp(g_drivers[i]->name, name) == 0) {
            return g_drivers[i];
        }
    }
    return nullptr;
}


__PRIVILEGED_CODE static mount_point* find_mount_for_instance(instance* inst) {
    for (uint32_t i = 0; i < g_mount_count; i++) {
        if (g_mounts[i] && g_mounts[i]->mounted_fs == inst) {
            return g_mounts[i];
        }
    }
    return nullptr;
}

__PRIVILEGED_CODE static void release_node_ref(node* n) {
    if (!n) {
        return;
    }
    if (n->release()) {
        node::ref_destroy(n);
    }
}

__PRIVILEGED_CODE static bool is_global_root_node(node* n) {
    return g_root_instance && n == g_root_instance->root();
}

__PRIVILEGED_CODE static int32_t acquire_global_root(node** out_root) {
    if (!out_root || !g_root_mount || !g_root_instance) {
        return ERR_INVAL;
    }

    node* cur = g_root_instance->root();
    if (!cur) {
        return ERR_INVAL;
    }
    cur->add_ref();

    while (cur->mounted_here()) {
        node* mounted_root = cur->mounted_here()->root();
        mounted_root->add_ref();
        release_node_ref(cur);
        cur = mounted_root;
    }

    *out_root = cur;
    return OK;
}

__PRIVILEGED_CODE static int32_t acquire_start_node(
    node* base_dir, const char* path, node** out_start
) {
    if (!path || !out_start) {
        return ERR_INVAL;
    }

    if (path[0] == '/') {
        return acquire_global_root(out_start);
    }

    node* start = base_dir;
    if (!start) {
        return acquire_global_root(out_start);
    }

    start->add_ref();
    while (start->mounted_here()) {
        node* mounted_root = start->mounted_here()->root();
        mounted_root->add_ref();
        release_node_ref(start);
        start = mounted_root;
    }

    *out_start = start;
    return OK;
}

__PRIVILEGED_CODE static int32_t resolve_path_at_internal(
    node* base_dir, const char* path, node** out
) {
    if (!path || !out) {
        return ERR_INVAL;
    }
    if (path[0] == '\0') {
        return ERR_NOENT;
    }

    node* cur = nullptr;
    int32_t start_err = acquire_start_node(base_dir, path, &cur);
    if (start_err != OK) {
        return start_err;
    }

    path_iterator it(path);
    const char* comp = nullptr;
    size_t comp_len = 0;

    while (it.next(comp, comp_len)) {
        if (comp_len > NAME_MAX) {
            release_node_ref(cur);
            return ERR_NAMETOOLONG;
        }

        if (comp_len == 2 && comp[0] == '.' && comp[1] == '.') {
            node* next = nullptr;
            if (cur->filesystem() && cur == cur->filesystem()->root()) {
                mount_point* mnt = find_mount_for_instance(cur->filesystem());
                if (mnt && mnt->mountpoint) {
                    next = mnt->mountpoint->parent();
                    if (!next) {
                        next = mnt->mountpoint;
                    }
                } else {
                    next = cur->parent() ? cur->parent() : cur;
                }
            } else {
                next = cur->parent() ? cur->parent() : cur;
            }

            next->add_ref();
            release_node_ref(cur);
            cur = next;
            continue;
        }

        if (cur->type() != node_type::directory) {
            release_node_ref(cur);
            return ERR_NOTDIR;
        }

        node* child = nullptr;
        int32_t err = cur->lookup(comp, comp_len, &child);
        if (err != OK) {
            release_node_ref(cur);
            return err;
        }

        while (child->mounted_here()) {
            node* mounted_root = child->mounted_here()->root();
            mounted_root->add_ref();
            release_node_ref(child);
            child = mounted_root;
        }

        release_node_ref(cur);
        cur = child;
    }

    *out = cur;
    return OK;
}

__PRIVILEGED_CODE static int32_t split_parent_path(
    const char* path,
    const char** out_name,
    size_t* out_name_len,
    size_t* out_parent_len
) {
    if (!path || !out_name || !out_name_len || !out_parent_len) {
        return ERR_INVAL;
    }

    size_t len = string::strnlen(path, PATH_MAX);
    if (len == 0) {
        return ERR_INVAL;
    }

    size_t end = len;
    while (end > 0 && path[end - 1] == '/') {
        end--;
    }
    if (end == 0) {
        return ERR_INVAL;
    }

    size_t name_start = end;
    while (name_start > 0 && path[name_start - 1] != '/') {
        name_start--;
    }

    size_t name_len = end - name_start;
    if (name_len > NAME_MAX) {
        return ERR_NAMETOOLONG;
    }

    *out_name = path + name_start;
    *out_name_len = name_len;
    *out_parent_len = name_start;
    return OK;
}

__PRIVILEGED_CODE static int32_t resolve_parent_at_internal(
    node* base_dir,
    const char* path,
    node** out_parent,
    const char** out_name,
    size_t* out_name_len
) {
    const char* name = nullptr;
    size_t name_len = 0;
    size_t parent_len = 0;

    int32_t err = split_parent_path(path, &name, &name_len, &parent_len);
    if (err != OK) {
        return err;
    }

    char parent_buf[PATH_MAX];
    if (parent_len >= PATH_MAX) {
        return ERR_NAMETOOLONG;
    }

    if (parent_len == 0) {
        parent_buf[0] = '.';
        parent_buf[1] = '\0';
    } else {
        string::memcpy(parent_buf, path, parent_len);
        parent_buf[parent_len] = '\0';
    }

    err = resolve_path_at_internal(base_dir, parent_buf, out_parent);
    if (err != OK) {
        return err;
    }

    if ((*out_parent)->type() != node_type::directory) {
        release_node_ref(*out_parent);
        *out_parent = nullptr;
        return ERR_NOTDIR;
    }

    *out_name = name;
    *out_name_len = name_len;
    return OK;
}

__PRIVILEGED_CODE int32_t path_from_node(
    node* target, char* out_path, size_t out_cap
) {
    if (!target || !out_path || out_cap == 0) {
        return ERR_INVAL;
    }
    if (!g_root_instance) {
        return ERR_INVAL;
    }

    char* path_buf = static_cast<char*>(heap::kzalloc(PATH_MAX));
    if (!path_buf) {
        return ERR_NOMEM;
    }
    size_t pos = PATH_MAX;
    path_buf[--pos] = '\0';

    node* cur = target;
    cur->add_ref();

    while (true) {
        if (is_global_root_node(cur)) {
            break;
        }

        if (cur->filesystem() && cur == cur->filesystem()->root()) {
            mount_point* mnt = find_mount_for_instance(cur->filesystem());
            if (!mnt || !mnt->mountpoint) {
                release_node_ref(cur);
                heap::kfree(path_buf);
                return ERR_NOENT;
            }

            node* next = mnt->mountpoint;
            next->add_ref();
            release_node_ref(cur);
            cur = next;
            continue;
        }

        const char* name = cur->name();
        size_t name_len = string::strnlen(name, NAME_MAX);
        if (name_len == 0) {
            release_node_ref(cur);
            heap::kfree(path_buf);
            return ERR_NOENT;
        }
        if (pos < name_len + 1) {
            release_node_ref(cur);
            heap::kfree(path_buf);
            return ERR_NAMETOOLONG;
        }

        pos -= name_len;
        string::memcpy(path_buf + pos, name, name_len);
        path_buf[--pos] = '/';

        node* parent = cur->parent();
        if (!parent) {
            release_node_ref(cur);
            heap::kfree(path_buf);
            return ERR_NOENT;
        }

        parent->add_ref();
        release_node_ref(cur);
        cur = parent;
    }

    release_node_ref(cur);

    if (pos == PATH_MAX - 1) {
        if (out_cap < 2) {
            heap::kfree(path_buf);
            return ERR_NAMETOOLONG;
        }
        out_path[0] = '/';
        out_path[1] = '\0';
        heap::kfree(path_buf);
        return OK;
    }

    size_t path_len = PATH_MAX - pos;
    if (out_cap < path_len) {
        heap::kfree(path_buf);
        return ERR_NAMETOOLONG;
    }

    string::memcpy(out_path, path_buf + pos, path_len);
    heap::kfree(path_buf);
    return OK;
}

__PRIVILEGED_CODE int32_t lookup(const char* path, node** out) {
    if (!path || !out) {
        return ERR_INVAL;
    }
    if (path[0] != '/') {
        return ERR_INVAL;
    }
    return resolve_path_at_internal(nullptr, path, out);
}

__PRIVILEGED_CODE int32_t lookup_at(node* base_dir, const char* path, node** out) {
    if (!path || !out) {
        return ERR_INVAL;
    }
    return resolve_path_at_internal(base_dir, path, out);
}

__PRIVILEGED_CODE int32_t resolve_parent_path(
    const char* path, node** out_parent,
    const char** out_name, size_t* out_name_len
) {
    if (!path || !out_parent || !out_name || !out_name_len) {
        return ERR_INVAL;
    }
    if (path[0] != '/') {
        return ERR_INVAL;
    }
    return resolve_parent_at_internal(
        nullptr, path, out_parent, out_name, out_name_len);
}

__PRIVILEGED_CODE int32_t resolve_parent_path_at(
    node* base_dir, const char* path, node** out_parent,
    const char** out_name, size_t* out_name_len
) {
    if (!path || !out_parent || !out_name || !out_name_len) {
        return ERR_INVAL;
    }
    return resolve_parent_at_internal(
        base_dir, path, out_parent, out_name, out_name_len);
}


__PRIVILEGED_CODE int32_t mount(const char* source, const char* target,
                                const char* fs_name, uint32_t flags) {
    if (!fs_name) return ERR_INVAL;

    driver* drv = find_driver(fs_name);
    if (!drv) {
        log::error("fs: unknown filesystem '%s'", fs_name);
        return ERR_NOENT;
    }

    instance* inst = nullptr;
    int32_t err = drv->mount_fn(drv, source, flags, nullptr, &inst);
    if (err != OK || !inst) {
        return err;
    }

    // Root mount (target is "/" and no root exists yet)
    if (target && target[0] == '/' && target[1] == '\0' && !g_root_instance) {
        auto* mnt = static_cast<mount_point*>(heap::kzalloc(sizeof(mount_point)));
        if (!mnt) {
            heap::kfree_delete(inst);
            return ERR_NOMEM;
        }

        mnt->mountpoint = nullptr;
        mnt->mounted_fs = inst;
        mnt->parent = nullptr;

        inst->root()->set_parent(inst->root());

        sync::irq_lock_guard guard(g_mount_lock);
        g_root_instance = inst;
        g_root_mount = mnt;
        if (g_mount_count < MAX_MOUNTS) {
            g_mounts[g_mount_count++] = mnt;
        }

        log::info("fs: mounted '%s' as rootfs", fs_name);
        return OK;
    }

    // Non-root mount: resolve target
    node* target_node = nullptr;
    err = lookup(target, &target_node);
    if (err != OK) {
        heap::kfree_delete(inst);
        return err;
    }

    if (target_node->type() != node_type::directory) {
        if (target_node->release()) {
            node::ref_destroy(target_node);
        }
        heap::kfree_delete(inst);
        return ERR_NOTDIR;
    }

    auto* mnt = static_cast<mount_point*>(heap::kzalloc(sizeof(mount_point)));
    if (!mnt) {
        if (target_node->release()) {
            node::ref_destroy(target_node);
        }
        heap::kfree_delete(inst);
        return ERR_NOMEM;
    }

    mnt->mountpoint = target_node;
    mnt->mounted_fs = inst;
    mnt->parent = g_root_mount;

    target_node->set_mounted_here(inst);

    sync::irq_lock_guard guard(g_mount_lock);
    if (g_mount_count < MAX_MOUNTS) {
        g_mounts[g_mount_count++] = mnt;
    }

    log::info("fs: mounted '%s' at %s", fs_name, target);
    return OK;
}

__PRIVILEGED_CODE int32_t unmount(const char* target) {
    (void)target;
    return ERR_NOSYS;
}


file* open(const char* path, uint32_t flags) {
    return open(path, flags, nullptr);
}

file* open(const char* path, uint32_t flags, int32_t* out_err) {
    auto set_err = [out_err](int32_t err) {
        if (out_err) {
            *out_err = err;
        }
    };

    if (!path || path[0] != '/') {
        set_err(ERR_INVAL);
        return nullptr;
    }

    node* n = nullptr;
    int32_t err = ERR_NOENT;

    RUN_ELEVATED({
        if (flags & O_CREAT) {
            node* parent = nullptr;
            const char* name;
            size_t name_len;

            err = resolve_parent_at_internal(
                nullptr, path, &parent, &name, &name_len);
            if (err == OK) {
                err = parent->lookup(name, name_len, &n);
                if (err == ERR_NOENT) {
                    err = parent->create(name, name_len, 0, &n);
                }
                if (err == ERR_EXIST) {
                    err = parent->lookup(name, name_len, &n);
                }
                if (parent->release()) {
                    node::ref_destroy(parent);
                }
            }
        } else {
            err = lookup(path, &n);
        }
    });

    if (err != OK || !n) {
        set_err((err != OK) ? err : ERR_IO);
        return nullptr;
    }

    // Allocate file from unprivileged heap
    // strong_ref::adopt just stores the pointer -- no privileged access
    void* mem = heap::uzalloc(sizeof(file));
    if (!mem) {
        RUN_ELEVATED({
            if (n->release()) {
                node::ref_destroy(n);
            }
        });
        set_err(ERR_NOMEM);
        return nullptr;
    }
    auto* f = new (mem) file(rc::strong_ref<node>::adopt(n), flags);

    // Call node->open for per-open setup
    RUN_ELEVATED({
        err = n->open(f, flags);
    });

    if (err != OK) {
        if (f->release()) {
            file::ref_destroy(f);
        }
        set_err(err);
        return nullptr;
    }

    f->mark_opened();
    set_err(OK);
    return f;
}

ssize_t read(file* f, void* buf, size_t count) {
    if (!f || !buf) return ERR_BADF;
    if (count == 0) return 0;
    uint32_t mode = f->flags() & ACCESS_MODE_MASK;
    if (mode == O_WRONLY) return ERR_BADF;

    ssize_t result;
    RUN_ELEVATED({
        result = f->get_node()->read(f, buf, count);
    });
    return result;
}

ssize_t write(file* f, const void* buf, size_t count) {
    if (!f || !buf) return ERR_BADF;
    if (count == 0) return 0;
    uint32_t mode = f->flags() & ACCESS_MODE_MASK;
    if (mode == O_RDONLY) return ERR_BADF;

    ssize_t result;
    RUN_ELEVATED({
        result = f->get_node()->write(f, buf, count);
    });
    return result;
}

int64_t seek(file* f, int64_t offset, int whence) {
    if (!f) return ERR_BADF;

    int64_t result;
    RUN_ELEVATED({
        result = f->get_node()->seek(f, offset, whence);
    });
    return result;
}

int32_t close(file* f) {
    if (!f) return ERR_BADF;
    if (f->release()) {
        file::ref_destroy(f);
    }
    return OK;
}

int32_t ioctl(file* f, uint32_t cmd, uint64_t arg) {
    if (!f) return ERR_BADF;

    int32_t result;
    RUN_ELEVATED({
        result = f->get_node()->ioctl(f, cmd, arg);
    });
    return result;
}

int32_t mmap(file* f, mm::mm_context* mm_ctx, uintptr_t addr, size_t length,
             uint32_t prot, uint32_t map_flags, uint64_t offset, uintptr_t* out_addr) {
    if (!f || !mm_ctx || !out_addr) return ERR_INVAL;

    int32_t result;
    RUN_ELEVATED({
        result = f->get_node()->mmap(f, mm_ctx, addr, length, prot, map_flags, offset, out_addr);
    });
    return result;
}

int32_t stat(const char* path, vattr* attr) {
    if (!path || !attr) return ERR_INVAL;

    node* n = nullptr;
    int32_t err;

    RUN_ELEVATED({
        err = lookup(path, &n);
        if (err == OK) {
            err = n->getattr(attr);
            if (n->release()) {
                node::ref_destroy(n);
            }
        }
    });
    return err;
}

int32_t fstat(file* f, vattr* attr) {
    if (!f || !attr) return ERR_BADF;

    int32_t result;
    RUN_ELEVATED({
        result = f->get_node()->getattr(attr);
    });
    return result;
}

int32_t mkdir(const char* path, uint32_t mode) {
    if (!path || path[0] != '/') return ERR_INVAL;

    int32_t err;
    RUN_ELEVATED({
        node* parent = nullptr;
        const char* name;
        size_t name_len;

        err = resolve_parent_at_internal(
            nullptr, path, &parent, &name, &name_len);
        if (err == OK) {
            node* child = nullptr;
            err = parent->mkdir(name, name_len, mode, &child);
            if (err == OK && child) {
                if (child->release()) {
                    node::ref_destroy(child);
                }
            }
            if (parent->release()) {
                node::ref_destroy(parent);
            }
        }
    });
    return err;
}

int32_t rmdir(const char* path) {
    if (!path || path[0] != '/') return ERR_INVAL;

    int32_t err;
    RUN_ELEVATED({
        node* parent = nullptr;
        const char* name;
        size_t name_len;

        err = resolve_parent_at_internal(
            nullptr, path, &parent, &name, &name_len);
        if (err == OK) {
            err = parent->rmdir(name, name_len);
            if (parent->release()) {
                node::ref_destroy(parent);
            }
        }
    });
    return err;
}

int32_t unlink(const char* path) {
    if (!path || path[0] != '/') return ERR_INVAL;

    int32_t err;
    RUN_ELEVATED({
        node* parent = nullptr;
        const char* name;
        size_t name_len;

        err = resolve_parent_at_internal(
            nullptr, path, &parent, &name, &name_len);
        if (err == OK) {
            err = parent->unlink(name, name_len);
            if (parent->release()) {
                node::ref_destroy(parent);
            }
        }
    });
    return err;
}

ssize_t readdir(file* f, dirent* entries, size_t count) {
    if (!f || !entries) return ERR_BADF;
    if (count == 0) return 0;

    ssize_t result;
    RUN_ELEVATED({
        result = f->get_node()->readdir(f, entries, count);
    });
    return result;
}


__PRIVILEGED_CODE int32_t init() {
    g_root_mount = nullptr;
    g_root_instance = nullptr;
    g_driver_count = 0;
    g_mount_count = 0;

    for (uint32_t i = 0; i < MAX_DRIVERS; i++) {
        g_drivers[i] = nullptr;
    }
    for (uint32_t i = 0; i < MAX_MOUNTS; i++) {
        g_mounts[i] = nullptr;
    }

    int32_t err = ::ramfs_init_driver();
    if (err != OK) {
        log::error("fs: ramfs driver registration failed");
        return err;
    }

    // Mount ramfs as rootfs
    err = mount(nullptr, "/", "ramfs", 0);
    if (err != OK) {
        log::error("fs: failed to mount rootfs");
        return err;
    }

    if (g_boot_info.module_count > 0) {
        int32_t cpio_err = cpio::load_initrd();
        if (cpio_err != cpio::OK) {
            log::warn("fs: initrd load failed (err=%d)", cpio_err);
        }
    }

    err = ::devfs_init_driver();
    if (err != OK) {
        log::error("fs: devfs driver registration failed");
        return err;
    }

    err = mkdir("/dev", 0);
    if (err != OK && err != ERR_EXIST) {
        log::error("fs: failed to create /dev mount point");
        return err;
    }

    err = mount(nullptr, "/dev", "devfs", 0);
    if (err != OK) {
        log::error("fs: failed to mount devfs at /dev");
        return err;
    }

    return OK;
}

} // namespace fs
