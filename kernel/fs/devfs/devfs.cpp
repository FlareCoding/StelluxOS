#include "fs/devfs/devfs.h"
#include "fs/dir_node.h"
#include "fs/mount.h"
#include "fs/fs.h"
#include "common/string.h"
#include "mm/heap.h"
#include "common/logging.h"

namespace devfs {

class devfs_dir_node : public fs::dir_node {
public:
    devfs_dir_node(fs::instance* fs, const char* name)
        : fs::dir_node(fs, name) {}

    void add_child(fs::node* child) {
        sync::irq_lock_guard guard(m_lock);
        attach_child(child);
    }
};

/* Built-in /dev/null: reads return EOF, writes are discarded. */
class devfs_null_node : public fs::node {
public:
    devfs_null_node(fs::instance* fs, const char* name)
        : fs::node(fs::node_type::char_device, fs, name) {}

    ssize_t read(fs::file*, void*, size_t) override { return 0; }

    ssize_t write(fs::file*, const void*, size_t count) override {
        return static_cast<ssize_t>(count);
    }
};

__PRIVILEGED_BSS static devfs_dir_node* g_devfs_root;
__PRIVILEGED_BSS static fs::instance*   g_devfs_instance;

__PRIVILEGED_CODE static int32_t devfs_mount_fn(
    fs::driver* drv, const char*, uint32_t, void*, fs::instance** out
) {
    void* root_mem = heap::kzalloc(sizeof(devfs_dir_node));
    if (!root_mem) return fs::ERR_NOMEM;

    auto* root = new (root_mem) devfs_dir_node(nullptr, "");

    void* inst_mem = heap::kzalloc(sizeof(fs::instance));
    if (!inst_mem) {
        root->~devfs_dir_node();
        heap::kfree(root);
        return fs::ERR_NOMEM;
    }

    auto* inst = new (inst_mem) fs::instance(drv, root);

    root->set_filesystem(inst);
    root->set_parent(root);

    g_devfs_root = root;
    g_devfs_instance = inst;

    void* null_mem = heap::kzalloc(sizeof(devfs_null_node));
    if (null_mem) {
        root->add_child(new (null_mem) devfs_null_node(inst, "null"));
    }

    *out = inst;
    return fs::OK;
}

__PRIVILEGED_DATA static fs::driver g_devfs_driver = {
    "devfs",
    devfs_mount_fn,
    {}
};

__PRIVILEGED_CODE int32_t init() {
    return fs::register_driver(&g_devfs_driver);
}

__PRIVILEGED_CODE int32_t add_char_device(const char*, fs::node* dev_node) {
    if (!g_devfs_root || !dev_node) {
        return ERR;
    }

    g_devfs_root->add_child(dev_node);
    return OK;
}

__PRIVILEGED_CODE fs::node* ensure_dir(const char* name) {
    if (!g_devfs_root || !name) {
        return nullptr;
    }

    size_t len = string::strlen(name);
    fs::node* existing = nullptr;
    if (g_devfs_root->lookup(name, len, &existing) == fs::OK) {
        return existing;
    }

    void* mem = heap::kzalloc(sizeof(devfs_dir_node));
    if (!mem) {
        return nullptr;
    }

    auto* dir = new (mem) devfs_dir_node(g_devfs_instance, name);
    g_devfs_root->add_child(dir);
    return dir;
}

__PRIVILEGED_CODE int32_t add_char_device_at(fs::node* dir, fs::node* dev_node) {
    if (!dir || !dev_node) {
        return ERR;
    }

    auto* ddir = static_cast<devfs_dir_node*>(dir);
    ddir->add_child(dev_node);
    return OK;
}

} // namespace devfs

extern "C" __PRIVILEGED_CODE int32_t devfs_init_driver() {
    return devfs::init();
}
