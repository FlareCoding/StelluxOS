#include "fs/devfs/devfs.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/mount.h"
#include "fs/fs.h"
#include "common/list.h"
#include "common/string.h"
#include "mm/heap.h"
#include "common/logging.h"

namespace devfs {

class devfs_dir_node : public fs::node {
public:
    devfs_dir_node(fs::instance* fs, const char* name)
        : fs::node(fs::node_type::directory, fs, name)
        , m_child_count(0) {
        m_children.init();
    }

    ~devfs_dir_node() override {
        while (!m_children.empty()) {
            fs::node* child = m_children.pop_front();
            child->set_parent(nullptr);
            if (child->release()) {
                fs::node::ref_destroy(child);
            }
        }
        m_child_count = 0;
    }

    int32_t lookup(const char* name, size_t len, fs::node** out) override {
        if (!name || !out) return fs::ERR_INVAL;

        sync::irq_lock_guard guard(m_lock);
        fs::node* child = find_child(name, len);
        if (!child) return fs::ERR_NOENT;

        child->add_ref();
        *out = child;
        return fs::OK;
    }

    ssize_t readdir(fs::file* f, fs::dirent* entries, size_t count) override {
        if (!f || !entries) return fs::ERR_BADF;
        if (count == 0) return 0;

        sync::irq_lock_guard guard(m_lock);

        size_t idx = static_cast<size_t>(f->offset());
        size_t written = 0;

        size_t cur_idx = 0;
        for (auto& child : m_children) {
            if (written >= count) break;
            if (cur_idx >= idx) {
                size_t name_len = string::strlen(child.name());
                if (name_len > fs::NAME_MAX) name_len = fs::NAME_MAX;
                string::memcpy(entries[written].name, child.name(), name_len);
                entries[written].name[name_len] = '\0';
                entries[written].type = child.type();
                written++;
            }
            cur_idx++;
        }

        f->set_offset(static_cast<int64_t>(idx + written));
        return static_cast<ssize_t>(written);
    }

    int32_t getattr(fs::vattr* attr) override {
        if (!attr) return fs::ERR_INVAL;
        attr->type = fs::node_type::directory;
        attr->size = m_child_count;
        return fs::OK;
    }

    void add_child(fs::node* child) {
        sync::irq_lock_guard guard(m_lock);
        child->set_parent(this);
        child->set_filesystem(m_fs);
        child->add_ref();
        m_children.push_back(child);
        m_child_count++;
    }

private:
    fs::node* find_child(const char* name, size_t len) {
        for (auto& child : m_children) {
            size_t child_len = string::strlen(child.name());
            if (child_len == len && string::strncmp(child.name(), name, len) == 0) {
                return &child;
            }
        }
        return nullptr;
    }

    list::head<fs::node, &fs::node::m_child_link> m_children;
    uint32_t m_child_count;
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

} // namespace devfs

extern "C" __PRIVILEGED_CODE int32_t devfs_init_driver() {
    return devfs::init();
}
