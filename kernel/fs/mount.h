#ifndef STELLUX_FS_MOUNT_H
#define STELLUX_FS_MOUNT_H

#include "common/list.h"
#include "fs/node.h"
#include "rc/strong_ref.h"

namespace fs {

/**
 * Filesystem type registration. Static lifetime (never freed).
 * Drivers register once and remain available for mounting.
 */
struct driver {
    const char* name;
    int32_t (*mount_fn)(driver* self, const char* source, uint32_t flags,
                        void* data, class instance** out);
    list::node link;
};

/**
 * A mounted filesystem instance. Owns the root node via strong_ref.
 * Lives in privileged memory (heap::kalloc).
 */
class instance {
public:
    instance(driver* drv, node* root);
    virtual ~instance() = default;

    node* root() const { return m_root.ptr(); }
    driver* fs_driver() const { return m_driver; }

    virtual int32_t unmount();

private:
    driver* m_driver;
    rc::strong_ref<node> m_root;

public:
    list::node m_link;
};

/**
 * A mount point binding. Links a directory in a parent filesystem
 * to the root of a child filesystem.
 * Lives in privileged memory (heap::kalloc).
 *
 * Named mount_point (not mount) to avoid shadowing the fs::mount() function.
 */
struct mount_point {
    node*     mountpoint;
    instance* mounted_fs;
    mount_point* parent;
    list::node link;
};

} // namespace fs

#endif // STELLUX_FS_MOUNT_H
