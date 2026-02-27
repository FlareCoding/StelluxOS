#ifndef STELLUX_FS_FILE_H
#define STELLUX_FS_FILE_H

#include "fs/node.h"
#include "rc/strong_ref.h"

namespace fs {

/**
 * An open file description. Tracks per-open state independent of the node.
 *
 * Files live in unprivileged memory (heap::ualloc). Fields like offset,
 * flags, and private_data can be read/written without elevation. Only
 * dereferencing m_node (which points to privileged memory) requires
 * elevation.
 *
 * file::ref_destroy() runs the entire teardown under RUN_ELEVATED so
 * that ~strong_ref<node> can safely release the privileged node.
 */
class file : public rc::ref_counted<file> {
public:
    file(rc::strong_ref<node>&& n, uint32_t flags);
    ~file() = default;

    node* get_node() const { return m_node.ptr(); }
    int64_t offset() const { return m_offset; }
    void set_offset(int64_t off) { m_offset = off; }
    uint32_t flags() const { return m_flags; }
    void* private_data() const { return m_private; }
    void set_private_data(void* data) { m_private = data; }
    bool opened() const { return m_opened; }
    void mark_opened() { m_opened = true; }

    static void ref_destroy(file* f);

private:
    rc::strong_ref<node> m_node;
    int64_t  m_offset;
    uint32_t m_flags;
    void*    m_private;
    bool     m_opened;
};

} // namespace fs

#endif // STELLUX_FS_FILE_H
