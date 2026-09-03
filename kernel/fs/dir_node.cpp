#include "fs/dir_node.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "common/string.h"

namespace fs {

// Every rename serializes on this lock, which keeps the ancestor walk that
// refuses to move a directory into its own subtree reading a stable tree and
// makes locking a replaced directory beneath its parents deadlock free
static sync::spinlock g_rename_lock = sync::SPINLOCK_INIT;

static bool is_dot_name(const char* name, size_t len) {
    return (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.');
}

// True when dir is ancestor itself or lies anywhere beneath it
static bool is_within(node* dir, node* ancestor) {
    for (node* n = dir; n; n = n->parent()) {
        if (n == ancestor) {
            return true;
        }

        if (n->parent() == n) {
            break;
        }
    }

    return false;
}

dir_node::dir_node(instance* fs, const char* name)
    : node(node_type::directory, fs, name)
    , m_child_count(0) {
    m_children.init();
}

dir_node::~dir_node() {
    // Destruction steals every directory's children into a flat worklist
    // first, so destructors never recurse and stack depth stays bounded.
    list::head<node, &node::m_child_link> worklist;
    worklist.init();

    while (!m_children.empty()) {
        node* child = m_children.pop_front();
        child->set_parent(nullptr);
        worklist.push_back(child);
    }
    m_child_count = 0;

    while (!worklist.empty()) {
        node* n = worklist.pop_front();
        if (n->type() == node_type::directory) {
            auto* dn = static_cast<dir_node*>(n);
            while (!dn->m_children.empty()) {
                node* grandchild = dn->m_children.pop_front();
                grandchild->set_parent(nullptr);
                worklist.push_back(grandchild);
            }
            dn->m_child_count = 0;
        }
        if (n->release()) {
            node::ref_destroy(n);
        }
    }
}

node* dir_node::find_child(const char* name, size_t len) {
    for (auto& child : m_children) {
        size_t child_len = string::strlen(child.name());
        if (child_len == len && string::strncmp(child.name(), name, len) == 0) {
            return &child;
        }
    }
    return nullptr;
}

void dir_node::attach_child(node* child) {
    child->set_parent(this);
    child->set_filesystem(m_fs);
    child->add_ref();
    m_children.push_back(child);
    m_child_count++;
    mark_modified();
}

void dir_node::detach_child(node* child) {
    m_children.remove(child);
    m_child_count--;
    child->set_parent(nullptr);
    mark_modified();

    if (child->release()) {
        node::ref_destroy(child);
    }
}

int32_t dir_node::rename_child(const char* name, size_t len, node* new_parent,
                               const char* new_name, size_t new_len) {
    if (!name || len == 0 || !new_parent || !new_name || new_len == 0) {
        return ERR_INVAL;
    }

    if (new_len > NAME_MAX) {
        return ERR_NAMETOOLONG;
    }

    if (is_dot_name(name, len) || is_dot_name(new_name, new_len)) {
        return ERR_INVAL;
    }

    if (new_parent->type() != node_type::directory) {
        return ERR_NOTDIR;
    }

    if (new_parent->filesystem() != m_fs) {
        return ERR_XDEV;
    }

    auto* dst = static_cast<dir_node*>(new_parent);
    sync::irq_lock_guard rename_guard(g_rename_lock);

    if (dst == this) {
        sync::irq_lock_guard guard(m_lock);
        return move_child_locked(name, len, dst, new_name, new_len);
    }

    // Two directories lock in address order so concurrent renames in
    // opposite directions cannot deadlock
    dir_node* first = reinterpret_cast<uintptr_t>(this) < reinterpret_cast<uintptr_t>(dst) ? this : dst;
    dir_node* second = first == this ? dst : this;

    sync::irq_lock_guard first_guard(first->m_lock);
    sync::irq_lock_guard second_guard(second->m_lock);

    return move_child_locked(name, len, dst, new_name, new_len);
}

// The directory's own lock is held from the emptiness check through the
// detach so no entry can appear in between
int32_t dir_node::detach_if_empty(dir_node* dir) {
    sync::irq_lock_guard guard(dir->m_lock);

    if (dir->mounted_here()) {
        return ERR_BUSY;
    }

    if (dir->m_child_count > 0) {
        return ERR_NOTEMPTY;
    }

    detach_child(dir);
    return OK;
}

int32_t dir_node::remove_empty_dir(dir_node* dir) {
    // A temporary reference keeps the directory alive while its own lock is
    // held across the detach, since the list reference may be its last
    dir->add_ref();
    int32_t rc = detach_if_empty(dir);
    if (dir->release()) {
        node::ref_destroy(dir);
    }

    return rc;
}

int32_t dir_node::replace_child_locked(node* child, node* existing) {
    bool child_is_dir = child->type() == node_type::directory;
    bool existing_is_dir = existing->type() == node_type::directory;

    if (existing_is_dir && !child_is_dir) {
        return ERR_ISDIR;
    }

    if (!existing_is_dir && child_is_dir) {
        return ERR_NOTDIR;
    }

    if (!existing_is_dir) {
        detach_child(existing);
        return OK;
    }

    return remove_empty_dir(static_cast<dir_node*>(existing));
}

int32_t dir_node::move_child_locked(const char* name, size_t len, dir_node* dst,
                                    const char* new_name, size_t new_len) {
    node* child = find_child(name, len);
    if (!child) {
        return ERR_NOENT;
    }

    if (child->mounted_here()) {
        return ERR_BUSY;
    }

    node* existing = dst->find_child(new_name, new_len);
    if (existing == child) {
        return OK;
    }

    if (child->type() == node_type::directory && is_within(dst, child)) {
        return ERR_INVAL;
    }

    if (existing) {
        int32_t rc = dst->replace_child_locked(child, existing);
        if (rc != OK) {
            return rc;
        }
    }

    // A temporary reference keeps the child alive between the two directories
    child->add_ref();
    detach_child(child);
    child->set_name(new_name, new_len);
    child->mark_changed();
    dst->attach_child(child);

    if (child->release()) {
        node::ref_destroy(child);
    }

    return OK;
}

int32_t dir_node::lookup(const char* name, size_t len, node** out) {
    if (!name || !out) return ERR_INVAL;

    sync::irq_lock_guard guard(m_lock);
    node* child = find_child(name, len);
    if (!child) return ERR_NOENT;

    child->add_ref();
    *out = child;
    return OK;
}

ssize_t dir_node::readdir(file* f, dirent* entries, size_t count) {
    if (!f || !entries) return ERR_BADF;

    if (count == 0) return 0;

    sync::irq_lock_guard guard(m_lock);

    size_t idx = static_cast<size_t>(f->offset());
    size_t written = 0;

    size_t cur_idx = 0;
    for (auto& child : m_children) {
        if (written >= count) {
            break;
        }

        if (cur_idx >= idx) {
            size_t name_len = string::strlen(child.name());
            if (name_len > NAME_MAX) {
                name_len = NAME_MAX;
            }
            string::memcpy(entries[written].name, child.name(), name_len);
            entries[written].name[name_len] = '\0';
            entries[written].type = child.type();
            entries[written].ino = child.ino();
            written++;
        }
        cur_idx++;
    }

    f->set_offset(static_cast<int64_t>(idx + written));
    return static_cast<ssize_t>(written);
}

int32_t dir_node::getattr(vattr* attr) {
    int32_t rc = node::getattr(attr);
    if (rc != OK) {
        return rc;
    }

    attr->size = m_child_count;
    return OK;
}

} // namespace fs
