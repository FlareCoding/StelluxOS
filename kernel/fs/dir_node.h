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

protected:
    // Callers hold m_lock across a find and the attach or detach it decides
    node* find_child(const char* name, size_t len);
    void attach_child(node* child);
    void detach_child(node* child);

    // Moves a child under new_parent as new_name, replacing an existing
    // entry there when the types allow. Takes every lock it needs itself.
    int32_t rename_child(const char* name, size_t len, node* new_parent,
                         const char* new_name, size_t new_len);

    // Caller holds m_lock. Detaches a child directory only while it is
    // provably empty, refusing mount points and populated directories.
    int32_t remove_empty_dir(dir_node* dir);

private:
    int32_t move_child_locked(const char* name, size_t len, dir_node* dst,
                              const char* new_name, size_t new_len);
    int32_t replace_child_locked(node* child, node* existing);
    int32_t detach_if_empty(dir_node* dir);

    list::head<node, &node::m_child_link> m_children;
    uint32_t m_child_count;
};

} // namespace fs

#endif // STELLUX_FS_DIR_NODE_H
