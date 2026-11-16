#include "fs/devfs/devfs.h"
#include "fs/dir_node.h"
#include "fs/file.h"
#include "fs/mount.h"
#include "fs/fs.h"
#include "common/string.h"
#include "mm/heap.h"
#include "common/logging.h"
#include "sync/poll.h"

namespace devfs {

constexpr size_t SNAPSHOT_INITIAL_CAPACITY = 4096;
constexpr size_t U64_DECIMAL_DIGITS_MAX    = 20;

// A snapshot's generation before its first read from offset zero, which no counter holds, since counting starts at 1
constexpr uint64_t NO_SNAPSHOT = 0;

class devfs_dir_node : public fs::dir_node {
public:
    devfs_dir_node(fs::instance* fs, const char* name)
        : fs::dir_node(fs, name) {}

    void add_child(fs::node* child) {
        sync::irq_lock_guard guard(m_lock);
        attach_child(child);
    }

    // False when `child` is not in this directory, as after a racing removal took it out
    bool remove_child(fs::node* child) {
        sync::irq_lock_guard guard(m_lock);
        if (child->parent() != this) {
            return false;
        }

        detach_child(child);

        return true;
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

__PRIVILEGED_CODE int32_t remove_char_device(fs::node* dev_node) {
    if (!dev_node || dev_node->type() != fs::node_type::char_device) {
        return ERR;
    }

    // Only devfs holds char_device nodes, and devfs never frees a directory, so the parent is read without its lock
    // and `remove_child` reads the parent again under that lock
    auto* dir = static_cast<devfs_dir_node*>(dev_node->parent());
    if (!dir || !dir->remove_child(dev_node)) {
        return ERR;
    }

    return OK;
}

void generation_counter::init() {
    m_value.store_relaxed(1);
    m_change_wq.init();
}

__PRIVILEGED_CODE uint64_t generation_counter::advance() {
    uint64_t generation = m_value.fetch_add_release(1) + 1;
    sync::wake_all(m_change_wq);
    return generation;
}

__PRIVILEGED_CODE void generation_counter::subscribe(sync::poll_table& pt) {
    sync::poll_subscribe(pt, m_change_wq);
}

int32_t text_snapshot_node::open(fs::file* f, uint32_t) {
    auto* snap = heap::ualloc_new<snapshot>();
    if (!snap) {
        return fs::ERR_NOMEM;
    }

    snap->text = static_cast<char*>(heap::ualloc(SNAPSHOT_INITIAL_CAPACITY));
    if (!snap->text) {
        heap::ufree_delete(snap);
        return fs::ERR_NOMEM;
    }

    snap->lock.init();
    snap->capacity = SNAPSHOT_INITIAL_CAPACITY;
    snap->generation.store_relaxed(NO_SNAPSHOT);

    f->set_private_data(snap);
    return fs::OK;
}

int32_t text_snapshot_node::on_close(fs::file* f) {
    auto* snap = static_cast<snapshot*>(f->private_data());
    if (snap) {
        f->set_private_data(nullptr);
        heap::ufree(snap->text);
        heap::ufree_delete(snap);
    }

    return fs::OK;
}

// Generates the text, growing the buffer until the whole text fits
int32_t text_snapshot_node::fill(snapshot* snap) {
    while (true) {
        size_t len = m_generate(snap->text, snap->capacity);
        if (len <= snap->capacity) {
            snap->len = len;
            return fs::OK;
        }

        auto* text = static_cast<char*>(heap::ualloc(len));
        if (!text) {
            return fs::ERR_NOMEM;
        }

        heap::ufree(snap->text);
        snap->text = text;
        snap->capacity = len;
    }
}

ssize_t text_snapshot_node::read(fs::file* f, void* buf, size_t count, uint32_t) {
    if (!f || !buf) {
        return fs::ERR_BADF;
    }

    auto* snap = static_cast<snapshot*>(f->private_data());
    if (!snap) {
        return fs::ERR_BADF;
    }

    sync::mutex_lock(snap->lock);
    ssize_t result = read_locked(f, snap, buf, count);
    sync::mutex_unlock(snap->lock);

    return result;
}

ssize_t text_snapshot_node::read_locked(fs::file* f, snapshot* snap, void* buf, size_t count) {
    int64_t off = f->offset();
    if (off < 0) {
        return fs::ERR_INVAL;
    }

    if (off == 0) {
        // Read before `fill`, so a change made while it runs leaves the snapshot stale
        uint64_t generation = m_generation ? m_generation->current() : NO_SNAPSHOT;
        int32_t rc = fill(snap);
        if (rc != fs::OK) {
            return rc;
        }

        snap->generation.store_release(generation);
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

int64_t text_snapshot_node::seek(fs::file* f, int64_t offset, int whence) {
    bool is_rewind = whence == fs::SEEK_SET && offset == 0;
    bool is_query = whence == fs::SEEK_CUR && offset == 0;
    if (!is_rewind && !is_query) {
        return fs::ERR_INVAL;
    }

    auto* snap = static_cast<snapshot*>(f->private_data());
    if (!snap) {
        return fs::ERR_BADF;
    }

    sync::mutex_lock(snap->lock);
    if (is_rewind) {
        f->set_offset(0);
    }

    int64_t result = f->offset();
    sync::mutex_unlock(snap->lock);

    return result;
}

uint32_t text_snapshot_node::poll(fs::file* f, sync::poll_table* pt) {
    if (!m_generation) {
        return fs::node::poll(f, pt);
    }

    // Subscribed before the generation is read, so a change after the read still wakes the poller
    if (pt) {
        m_generation->subscribe(*pt);
    }

    auto* snap = static_cast<snapshot*>(f->private_data());
    bool is_stale = !snap || snap->generation.load_acquire() != m_generation->current();
    return is_stale ? sync::POLL_IN : 0;
}

static size_t append_bytes(char* buf, size_t cap, size_t pos, const char* bytes, size_t length) {
    for (size_t i = 0; i < length; i++, pos++) {
        if (pos < cap) {
            buf[pos] = bytes[i];
        }
    }

    return pos;
}

size_t append_str(char* buf, size_t cap, size_t pos, const char* s) {
    return append_bytes(buf, cap, pos, s, string::strlen(s));
}

size_t append_u64(char* buf, size_t cap, size_t pos, uint64_t value) {
    char digits[U64_DECIMAL_DIGITS_MAX];
    size_t length = string::format_u64(digits, sizeof(digits), value);
    return append_bytes(buf, cap, pos, digits, length);
}

} // namespace devfs

extern "C" __PRIVILEGED_CODE int32_t devfs_init_driver() {
    return devfs::init();
}
