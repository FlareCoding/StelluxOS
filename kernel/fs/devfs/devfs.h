#ifndef STELLUX_FS_DEVFS_DEVFS_H
#define STELLUX_FS_DEVFS_DEVFS_H

#include "common/types.h"
#include "fs/node.h"
#include "sync/atomic.h"
#include "sync/mutex.h"
#include "sync/wait_queue.h"

namespace devfs {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief A snapshot node's generation, the count of changes to its text. The
 * text's owner must `init` it before adding the node, must call `advance` after
 * each change the node's pollers wait for, and must keep it until the node and
 * every file and poll on it are gone. A change no poller waits for, such as a
 * count the text shows, needs no `advance`.
 */
class generation_counter {
public:
    void init();

    // Read with acquire ordering, so text built afterwards holds every change counted so far
    uint64_t current() const { return m_value.load_acquire(); }

    /**
     * @brief Counts a change already made to the text and wakes the node's
     * pollers. Never blocks, so the owner may call it under its spinlock.
     * @return The generation that counts the change.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint64_t advance();

    /**
     * @brief Has every later `advance` wake `pt`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void subscribe(sync::poll_table& pt);

private:
    sync::atomic<uint64_t> m_value;
    sync::wait_queue       m_change_wq;
};

/**
 * @brief A readable devfs text node. Every open holds a private snapshot buffer,
 * a read from offset zero regenerates it and later reads serve the same
 * bytes, so each reader sees one consistent capture. A node given a generation,
 * which must outlive it, reports POLL_IN to a file that has taken no snapshot
 * or whose snapshot predates the latest change, so a reader reads its snapshot
 * to the end before it polls. A seek only goes back to offset zero or reports
 * the offset.
 */
class text_snapshot_node : public fs::node {
public:
    // Writes the text into buf as far as cap allows and returns its full length,
    // so the node can grow the buffer and generate again
    using generator = size_t (*)(char* buf, size_t cap);

    text_snapshot_node(const char* name, generator gen, generation_counter* generation = nullptr)
        : fs::node(fs::node_type::char_device, nullptr, name), m_generate(gen), m_generation(generation) {}

    int32_t open(fs::file* f, uint32_t flags) override;
    int32_t on_close(fs::file* f) override;
    ssize_t read(fs::file* f, void* buf, size_t count, uint32_t flags) override;
    int64_t seek(fs::file* f, int64_t offset, int whence) override;
    uint32_t poll(fs::file* f, sync::poll_table* pt) override;

private:
    struct snapshot {
        sync::mutex            lock;       // held by each read and seek, since threads and forks share one open file
        char*                  text;
        size_t                 capacity;
        size_t                 len;
        sync::atomic<uint64_t> generation; // the node's generation when the text was taken
    };

    int32_t fill(snapshot* snap);
    ssize_t read_locked(fs::file* f, snapshot* snap, void* buf, size_t count);

    generator           m_generate;
    generation_counter* m_generation;
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

/**
 * @brief Remove a char_device node from its devfs directory, without blocking.
 * Lookups and listings stop finding the node at once, while open files keep
 * it allocated and keep calling it. The caller must hold its own reference to
 * `dev_node` and release it after the call. The node's driver must keep what
 * the node's methods use until the node's destructor runs, on whichever task
 * drops the last reference.
 * @return OK, or ERR when `dev_node` is not a char_device in a devfs directory.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t remove_char_device(fs::node* dev_node);

// Text append helpers for generators. They write only what fits below cap but
// return the position after the whole text, so a generator returns its full length.
size_t append_str(char* buf, size_t cap, size_t pos, const char* s);
size_t append_u64(char* buf, size_t cap, size_t pos, uint64_t value);

} // namespace devfs

#endif // STELLUX_FS_DEVFS_DEVFS_H
