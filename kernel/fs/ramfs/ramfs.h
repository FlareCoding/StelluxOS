#ifndef STELLUX_FS_RAMFS_RAMFS_H
#define STELLUX_FS_RAMFS_RAMFS_H

#include "fs/node.h"
#include "fs/file.h"
#include "fs/mount.h"
#include "fs/fs.h"
#include "common/list.h"

namespace ramfs {

/**
 * @brief Register the ramfs driver with the VFS.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

class dir_node : public fs::node {
public:
    dir_node(fs::instance* fs, const char* name);
    ~dir_node() override;

    int32_t lookup(const char* name, size_t len, fs::node** out) override;
    int32_t create(const char* name, size_t len, uint32_t mode, fs::node** out) override;
    int32_t mkdir(const char* name, size_t len, uint32_t mode, fs::node** out) override;
    int32_t unlink(const char* name, size_t len) override;
    int32_t rmdir(const char* name, size_t len) override;
    ssize_t readdir(fs::file* f, fs::dirent* entries, size_t count) override;
    int32_t getattr(fs::vattr* attr) override;
    int32_t create_socket(const char* name, size_t len, void* impl, fs::node** out) override;

private:
    fs::node* find_child(const char* name, size_t len);

    list::head<fs::node, &fs::node::m_child_link> m_children;
    uint32_t m_child_count;
};

class file_node : public fs::node {
public:
    file_node(fs::instance* fs, const char* name);
    ~file_node() override;

    ssize_t read(fs::file* f, void* buf, size_t count) override;
    ssize_t write(fs::file* f, const void* buf, size_t count) override;
    int64_t seek(fs::file* f, int64_t offset, int whence) override;
    int32_t getattr(fs::vattr* attr) override;
    int32_t truncate(size_t size) override;

private:
    int32_t ensure_capacity(uint32_t needed_pages);

    uint8_t** m_pages;
    uint32_t  m_page_count;
    uint32_t  m_capacity;
};

} // namespace ramfs

#endif // STELLUX_FS_RAMFS_RAMFS_H
