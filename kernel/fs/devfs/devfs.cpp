#include "fs/devfs/devfs.h"
#include "fs/dir_node.h"
#include "fs/file.h"
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

    ssize_t read(fs::file*, void*, size_t, uint32_t) override { return 0; }

    ssize_t write(fs::file*, const void*, size_t count, uint32_t) override {
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

int32_t text_snapshot_node::open(fs::file* f, uint32_t) {
    void* mem = heap::uzalloc(sizeof(snapshot) + m_cap);
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* snap = static_cast<snapshot*>(mem);
    snap->text = reinterpret_cast<char*>(mem) + sizeof(snapshot);
    snap->len = 0;

    f->set_private_data(snap);
    return fs::OK;
}

int32_t text_snapshot_node::on_close(fs::file* f) {
    auto* snap = static_cast<snapshot*>(f->private_data());
    if (snap) {
        f->set_private_data(nullptr);
        heap::ufree(snap);
    }

    return fs::OK;
}

ssize_t text_snapshot_node::read(fs::file* f, void* buf, size_t count, uint32_t) {
    if (!f || !buf) {
        return fs::ERR_BADF;
    }

    auto* snap = static_cast<snapshot*>(f->private_data());
    if (!snap) {
        return fs::ERR_BADF;
    }

    int64_t off = f->offset();
    if (off < 0) {
        return fs::ERR_INVAL;
    }

    if (off == 0) {
        snap->len = m_generate(snap->text, m_cap);
    }

    size_t offset = static_cast<size_t>(off);
    if (offset >= snap->len) {
        return 0;
    }

    if (offset + count > snap->len) {
        count = snap->len - offset;
    }

    string::memcpy(buf, snap->text + offset, count);
    f->set_offset(static_cast<int64_t>(offset + count));
    return static_cast<ssize_t>(count);
}

size_t append_str(char* buf, size_t cap, size_t pos, const char* s) {
    while (*s && pos < cap) {
        buf[pos++] = *s++;
    }

    return pos;
}

size_t append_u64(char* buf, size_t cap, size_t pos, uint64_t value) {
    size_t digits = string::format_u64(buf + pos, cap - pos, value);
    return digits > 0 ? pos + digits : cap;
}

} // namespace devfs

extern "C" __PRIVILEGED_CODE int32_t devfs_init_driver() {
    return devfs::init();
}
