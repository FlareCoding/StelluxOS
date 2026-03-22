#ifndef STELLUX_FS_NODE_H
#define STELLUX_FS_NODE_H

#include "fs/fstypes.h"
#include "rc/ref_counted.h"
#include "sync/spinlock.h"
#include "common/list.h"

namespace mm { struct mm_context; }

namespace fs {

class file;
class instance;

/**
 * Virtual base class representing any filesystem object.
 * Every file, directory, device node, and symlink is an fs::node.
 *
 * Nodes live in privileged memory (heap::kalloc). All access to node
 * fields requires elevated privilege. Virtual methods execute in the
 * caller's privilege context.
 *
 * Default implementations return ERR_NOSYS for unsupported operations
 * and OK (0) for lifecycle hooks.
 */
class node : public rc::ref_counted<node> {
public:
    node(node_type t, instance* fs, const char* name);
    virtual ~node() = default;

    // --- Namespace ops (directory nodes override) ---
    virtual int32_t lookup(const char* name, size_t len, node** out);
    virtual int32_t create(const char* name, size_t len, uint32_t mode, node** out);
    virtual int32_t mkdir(const char* name, size_t len, uint32_t mode, node** out);
    virtual int32_t unlink(const char* name, size_t len);
    virtual int32_t rmdir(const char* name, size_t len);

    // --- I/O ops (file/device nodes override) ---
    virtual ssize_t read(file* f, void* buf, size_t count);
    virtual ssize_t write(file* f, const void* buf, size_t count);
    virtual int64_t seek(file* f, int64_t offset, int whence);
    virtual ssize_t readdir(file* f, dirent* entries, size_t count);
    virtual int32_t ioctl(file* f, uint32_t cmd, uint64_t arg);
    virtual int32_t mmap(file* f, mm::mm_context* mm_ctx, uintptr_t addr,
                         size_t length, uint32_t prot, uint32_t map_flags,
                         uint64_t offset, uintptr_t* out_addr);

    // --- Lifecycle hooks (called on open/close) ---
    virtual int32_t open(file* f, uint32_t flags);
    virtual int32_t on_close(file* f);

    // --- Metadata ---
    virtual int32_t getattr(vattr* attr);
    virtual int32_t truncate(size_t size);

    // --- Symlink ---
    virtual int32_t readlink(char* buf, size_t size, size_t* out_len);

    // --- Socket node creation (directory nodes may override) ---
    virtual int32_t create_socket(const char* name, size_t len, void* impl, node** out);

    /**
     * ref_counted contract. Destroys the node and frees privileged memory.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(node* n);

    // --- Accessors ---
    node_type type() const { return m_type; }
    instance* filesystem() const { return m_fs; }
    node* parent() const { return m_parent; }
    const char* name() const { return m_name; }
    size_t size() const { return m_size; }
    instance* mounted_here() const { return m_mounted_here; }

    void set_parent(node* p) { m_parent = p; }
    void set_filesystem(instance* fs) { m_fs = fs; }
    void set_mounted_here(instance* inst) { m_mounted_here = inst; }

    list::node     m_child_link;

protected:
    node_type      m_type;
    instance*      m_fs;
    node*          m_parent;
    char           m_name[NAME_MAX + 1];
    size_t         m_size;
    sync::spinlock m_lock;
    instance*      m_mounted_here;
};

} // namespace fs

#endif // STELLUX_FS_NODE_H
