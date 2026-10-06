#ifndef STELLUX_FS_DEVFS_DEVFS_H
#define STELLUX_FS_DEVFS_DEVFS_H

#include "common/types.h"
#include "fs/node.h"

namespace devfs {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief A readable devfs text node. Every open holds a private snapshot buffer,
 * a read from offset zero regenerates it and later reads serve the same
 * bytes, so each reader sees one consistent capture.
 */
class text_snapshot_node : public fs::node {
public:
    using generator = size_t (*)(char* buf, size_t cap);

    text_snapshot_node(const char* name, generator gen, size_t cap)
        : fs::node(fs::node_type::char_device, nullptr, name),
          m_generate(gen), m_cap(cap) {}

    int32_t open(fs::file* f, uint32_t flags) override;
    int32_t on_close(fs::file* f) override;
    ssize_t read(fs::file* f, void* buf, size_t count, uint32_t flags) override;

private:
    struct snapshot {
        char*  text;
        size_t len;
    };

    generator m_generate;
    size_t    m_cap;
};

/**
 * @brief Register the devfs driver with the VFS.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Add a char_device node to the devfs root directory.
 * Must be called after devfs is mounted. The node's parent and
 * filesystem are set automatically.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t add_char_device(const char* name, fs::node* dev_node);

/**
 * @brief Ensure a subdirectory exists under the devfs root.
 * Creates it if it does not already exist. Returns a pointer
 * to the directory node on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE fs::node* ensure_dir(const char* name);

/**
 * @brief Add a char_device node under a specific devfs directory.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t add_char_device_at(fs::node* dir, fs::node* dev_node);

// Bounded text append helpers. Output past cap is dropped silently, so
// an overfull snapshot ends with a truncated final line.
size_t append_str(char* buf, size_t cap, size_t pos, const char* s);
size_t append_u64(char* buf, size_t cap, size_t pos, uint64_t value);

} // namespace devfs

#endif // STELLUX_FS_DEVFS_DEVFS_H
