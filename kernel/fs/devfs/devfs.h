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
    // Writes the text into buf as far as cap allows and returns its full length,
    // so the node can grow the buffer and generate again
    using generator = size_t (*)(char* buf, size_t cap);

    text_snapshot_node(const char* name, generator gen)
        : fs::node(fs::node_type::char_device, nullptr, name), m_generate(gen) {}

    int32_t open(fs::file* f, uint32_t flags) override;
    int32_t on_close(fs::file* f) override;
    ssize_t read(fs::file* f, void* buf, size_t count, uint32_t flags) override;

private:
    struct snapshot {
        char*  text;
        size_t capacity;
        size_t len;
    };

    int32_t fill(snapshot* snap);

    generator m_generate;
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

// Text append helpers for generators. They write only what fits below cap but
// return the position after the whole text, so a generator returns its full length.
size_t append_str(char* buf, size_t cap, size_t pos, const char* s);
size_t append_u64(char* buf, size_t cap, size_t pos, uint64_t value);

} // namespace devfs

#endif // STELLUX_FS_DEVFS_DEVFS_H
