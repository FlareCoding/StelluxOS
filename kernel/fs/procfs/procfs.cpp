#include "fs/procfs/procfs.h"
#include "fs/dir_node.h"
#include "fs/mount.h"
#include "fs/fs.h"
#include "common/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/task.h"

namespace procfs {

// Marks the path of a program whose file was unlinked after it started
constexpr char DELETED_SUFFIX[] = " (deleted)";

class procfs_dir_node : public fs::dir_node {
public:
    procfs_dir_node(fs::instance* fs, const char* name)
        : fs::dir_node(fs, name) {}

    void add_child(fs::node* child) {
        sync::irq_lock_guard guard(m_lock);
        attach_child(child);
    }
};

// Names the program of whichever process reads the link
class exe_link_node : public fs::node {
public:
    exe_link_node(fs::instance* fs, const char* name)
        : fs::node(fs::node_type::symlink, fs, name) {}

    int32_t readlink(char* buf, size_t size, size_t* out_len) override {
        return exe_path(sched::current(), buf, size, out_len);
    }
};

__PRIVILEGED_CODE static int32_t procfs_mount_fn(
    fs::driver* drv, const char*, uint32_t, void*, fs::instance** out
) {
    void* root_mem = heap::kzalloc(sizeof(procfs_dir_node));
    void* self_mem = heap::kzalloc(sizeof(procfs_dir_node));
    void* exe_mem = heap::kzalloc(sizeof(exe_link_node));
    void* inst_mem = heap::kzalloc(sizeof(fs::instance));
    if (!root_mem || !self_mem || !exe_mem || !inst_mem) {
        void* allocations[] = { root_mem, self_mem, exe_mem, inst_mem };
        for (void* mem : allocations) {
            if (mem) {
                heap::kfree(mem);
            }
        }

        return fs::ERR_NOMEM;
    }

    auto* root = new (root_mem) procfs_dir_node(nullptr, "");
    auto* inst = new (inst_mem) fs::instance(drv, root);
    root->set_filesystem(inst);
    root->set_parent(root);

    auto* self = new (self_mem) procfs_dir_node(inst, "self");
    self->add_child(new (exe_mem) exe_link_node(inst, "exe"));
    root->add_child(self);

    *out = inst;
    return fs::OK;
}

__PRIVILEGED_DATA static fs::driver g_procfs_driver = {
    "procfs",
    procfs_mount_fn,
    {}
};

__PRIVILEGED_CODE int32_t init() {
    return fs::register_driver(&g_procfs_driver);
}

static int32_t name_deleted_program(const char* started_path, char* out, size_t cap) {
    size_t len = string::strlen(started_path);
    size_t suffix_len = sizeof(DELETED_SUFFIX) - 1;
    if (len + suffix_len >= cap) {
        return fs::ERR_NAMETOOLONG;
    }

    string::memcpy(out, started_path, len);
    string::memcpy(out + len, DELETED_SUFFIX, suffix_len + 1);
    return fs::OK;
}

__PRIVILEGED_CODE int32_t exe_path(sched::task* task, char* buf, size_t size, size_t* out_len) {
    if (!task->group || !task->group->program) {
        return fs::ERR_NOENT;
    }

    auto* path = static_cast<char*>(heap::ualloc(fs::PATH_MAX));
    if (!path) {
        return fs::ERR_NOMEM;
    }

    // A program whose file was unlinked has no path left, so the one it started from stands in
    int32_t rc = fs::path_from_node(task->group->program, path, fs::PATH_MAX);
    if (rc == fs::ERR_NOENT && task->group->program_path) {
        rc = name_deleted_program(task->group->program_path, path, fs::PATH_MAX);
    }

    if (rc == fs::OK) {
        size_t len = string::strnlen(path, fs::PATH_MAX);
        size_t copied = len < size ? len : size;
        string::memcpy(buf, path, copied);
        *out_len = copied;
    }

    heap::ufree(path);

    return rc;
}

} // namespace procfs

extern "C" __PRIVILEGED_CODE int32_t procfs_init_driver() {
    return procfs::init();
}
