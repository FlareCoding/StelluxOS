#ifndef STELLUX_FS_DIR_NODE_H
#define STELLUX_FS_DIR_NODE_H

#include "fs/node.h"
#include "common/list.h"

namespace fs {

/**
 * Base class for in-memory directory nodes. Owns the child list and the
 * lookups over it, while each filesystem decides which mutations it allows:
 * ramfs exposes create, mkdir, unlink, and rmdir, devfs is populated only by
 * kernel drivers. Every directory node derives from this class, which the
 * destructor relies on to tear down nested directories without recursion.
 */
class dir_node : public node {
public:
    dir_node(instance* fs, const char* name);
    ~dir_node() override;

    int32_t lookup(const char* name, size_t len, node** out) override;
    ssize_t readdir(file* f, dirent* entries, size_t count) override;
    int32_t getattr(vattr* attr) override;

    uint32_t child_count() const { return m_child_count; }

protected:
    // Callers hold m_lock across a find and the attach or detach it decides
    node* find_child(const char* name, size_t len);
    void attach_child(node* child);
    void detach_child(node* child);

private:
    list::head<node, &node::m_child_link> m_children;
    uint32_t m_child_count;
};

} // namespace fs

#endif // STELLUX_FS_DIR_NODE_H
