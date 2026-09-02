#include "fs/dir_node.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "common/string.h"

namespace fs {

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
}

void dir_node::detach_child(node* child) {
    m_children.remove(child);
    m_child_count--;
    child->set_parent(nullptr);

    if (child->release()) {
        node::ref_destroy(child);
    }
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
