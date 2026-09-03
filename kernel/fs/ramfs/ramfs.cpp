#include "fs/ramfs/ramfs.h"
#include "fs/socket_node.h"
#include "fs/fs.h"
#include "common/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "boot/boot_services.h"

namespace ramfs {

__PRIVILEGED_CODE static int32_t ramfs_mount_fn(
    fs::driver* drv, const char* source, uint32_t flags,
    void* data, fs::instance** out
) {
    (void)drv; (void)source; (void)flags; (void)data;

    void* root_mem = heap::kzalloc(sizeof(dir_node));
    if (!root_mem) return fs::ERR_NOMEM;

    auto* root = new (root_mem) dir_node(nullptr, "");

    void* inst_mem = heap::kzalloc(sizeof(fs::instance));
    if (!inst_mem) {
        root->~dir_node();
        heap::kfree(root);
        return fs::ERR_NOMEM;
    }

    auto* inst = new (inst_mem) fs::instance(drv, root);

    root->set_filesystem(inst);
    root->set_parent(root);

    *out = inst;
    return fs::OK;
}

__PRIVILEGED_DATA static fs::driver g_ramfs_driver = {
    "ramfs",
    ramfs_mount_fn,
    {}
};

__PRIVILEGED_CODE int32_t init() {
    return fs::register_driver(&g_ramfs_driver);
}

} // namespace ramfs

// Called from fs::init() to register the ramfs driver
extern "C" __PRIVILEGED_CODE int32_t ramfs_init_driver() {
    return ramfs::init();
}

namespace ramfs {

dir_node::dir_node(fs::instance* fs, const char* name)
    : fs::dir_node(fs, name) {
}

int32_t dir_node::create(const char* name, size_t len, uint32_t mode, fs::node** out) {
    if (!name || !out || len == 0) return fs::ERR_INVAL;

    if (len > fs::NAME_MAX) return fs::ERR_NAMETOOLONG;

    (void)mode;

    sync::irq_lock_guard guard(m_lock);

    if (find_child(name, len)) {
        return fs::ERR_EXIST;
    }

    char name_buf[fs::NAME_MAX + 1];
    string::memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    void* mem = heap::kzalloc(sizeof(file_node));
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* child = new (mem) file_node(m_fs, name_buf);
    attach_child(child);

    *out = child;
    return fs::OK;
}

int32_t dir_node::create_socket(const char* name, size_t len, void* impl, fs::node** out) {
    (void)impl;
    if (!name || !out || len == 0) return fs::ERR_INVAL;

    if (len > fs::NAME_MAX) return fs::ERR_NAMETOOLONG;

    sync::irq_lock_guard guard(m_lock);

    if (find_child(name, len)) {
        return fs::ERR_EXIST;
    }

    char name_buf[fs::NAME_MAX + 1];
    string::memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    void* mem = heap::kzalloc(sizeof(fs::socket_node));
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* child = new (mem) fs::socket_node(m_fs, name_buf);
    attach_child(child);

    *out = child;
    return fs::OK;
}

int32_t dir_node::mkdir(const char* name, size_t len, uint32_t mode, fs::node** out) {
    if (!name || !out || len == 0) return fs::ERR_INVAL;

    if (len > fs::NAME_MAX) return fs::ERR_NAMETOOLONG;

    (void)mode;

    sync::irq_lock_guard guard(m_lock);

    if (find_child(name, len)) {
        return fs::ERR_EXIST;
    }

    char name_buf[fs::NAME_MAX + 1];
    string::memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    void* mem = heap::kzalloc(sizeof(dir_node));
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* child = new (mem) dir_node(m_fs, name_buf);
    attach_child(child);

    *out = child;
    return fs::OK;
}

int32_t dir_node::unlink(const char* name, size_t len) {
    if (!name || len == 0) return fs::ERR_INVAL;

    sync::irq_lock_guard guard(m_lock);

    fs::node* child = find_child(name, len);
    if (!child) {
        return fs::ERR_NOENT;
    }

    if (child->type() == fs::node_type::directory) {
        return fs::ERR_ISDIR;
    }

    detach_child(child);
    return fs::OK;
}

int32_t dir_node::rmdir(const char* name, size_t len) {
    return rmdir_child(name, len);
}

int32_t dir_node::rename(const char* name, size_t len, fs::node* new_parent,
                         const char* new_name, size_t new_len) {
    return rename_child(name, len, new_parent, new_name, new_len);
}

int32_t dir_node::symlink(const char* name, size_t len, const char* target, fs::node** out) {
    if (!name || !out || !target || len == 0) {
        return fs::ERR_INVAL;
    }

    if (len > fs::NAME_MAX) {
        return fs::ERR_NAMETOOLONG;
    }

    sync::irq_lock_guard guard(m_lock);

    if (find_child(name, len)) {
        return fs::ERR_EXIST;
    }

    char name_buf[fs::NAME_MAX + 1];
    string::memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    void* mem = heap::kzalloc(sizeof(symlink_node));
    if (!mem) {
        return fs::ERR_NOMEM;
    }

    auto* child = new (mem) symlink_node(m_fs, name_buf);
    int32_t rc = child->set_target(target);
    if (rc != fs::OK) {
        fs::node::ref_destroy(child);
        return rc;
    }

    attach_child(child);

    *out = child;
    return fs::OK;
}

symlink_node::symlink_node(fs::instance* fs, const char* name)
    : fs::node(fs::node_type::symlink, fs, name)
    , m_target(nullptr)
    , m_target_len(0) {
}

symlink_node::~symlink_node() {
    if (m_target) {
        heap::kfree(m_target);
    }
}

int32_t symlink_node::set_target(const char* target) {
    size_t len = string::strnlen(target, fs::PATH_MAX);
    if (len == 0 || len >= fs::PATH_MAX) {
        return fs::ERR_INVAL;
    }

    auto* copy = static_cast<char*>(heap::kzalloc(len + 1));
    if (!copy) {
        return fs::ERR_NOMEM;
    }

    string::memcpy(copy, target, len);
    m_target = copy;
    m_target_len = len;
    m_size = len;

    return fs::OK;
}

int32_t symlink_node::readlink(char* buf, size_t size, size_t* out_len) {
    if (!buf || !out_len) {
        return fs::ERR_INVAL;
    }

    size_t n = m_target_len < size ? m_target_len : size;
    string::memcpy(buf, m_target, n);
    *out_len = n;

    return fs::OK;
}

file_node::file_node(fs::instance* fs, const char* name)
    : fs::node(fs::node_type::regular, fs, name)
    , m_pages(nullptr)
    , m_page_count(0)
    , m_capacity(0) {
}

file_node::~file_node() {
    if (m_pages) {
        for (uint32_t i = 0; i < m_page_count; i++) {
            if (m_pages[i]) {
                pmm::phys_addr_t phys =
                    reinterpret_cast<uintptr_t>(m_pages[i]) - g_boot_info.hhdm_offset;
                pmm::free_page(phys);
            }
        }
        heap::kfree(m_pages);
        m_pages = nullptr;
    }
    m_page_count = 0;
    m_capacity = 0;
}

int32_t file_node::ensure_capacity(uint32_t needed_pages) {
    if (needed_pages <= m_capacity) return fs::OK;

    uint32_t new_cap = m_capacity ? m_capacity : 4;
    while (new_cap < needed_pages) {
        if (new_cap > 0x80000000u) {
            return fs::ERR_NOMEM;
        }

        new_cap *= 2;
    }

    auto* new_pages = static_cast<uint8_t**>(heap::kzalloc(new_cap * sizeof(uint8_t*)));
    if (!new_pages) {
        return fs::ERR_NOMEM;
    }

    if (m_pages) {
        string::memcpy(new_pages, m_pages, m_page_count * sizeof(uint8_t*));
        heap::kfree(m_pages);
    }

    m_pages = new_pages;
    m_capacity = new_cap;
    return fs::OK;
}

ssize_t file_node::read(fs::file* f, void* buf, size_t count) {
    if (!f || !buf) return fs::ERR_BADF;

    sync::irq_lock_guard guard(m_lock);

    int64_t off = f->offset();
    if (off < 0) return fs::ERR_INVAL;

    size_t offset = static_cast<size_t>(off);
    if (offset >= m_size) {
        return 0;
    }

    if (offset + count > m_size) {
        count = m_size - offset;
    }

    auto* dst = static_cast<uint8_t*>(buf);
    size_t remaining = count;
    size_t pos = offset;

    while (remaining > 0) {
        uint32_t page_idx = static_cast<uint32_t>(pos / pmm::PAGE_SIZE);
        size_t page_off = pos % pmm::PAGE_SIZE;
        size_t chunk = pmm::PAGE_SIZE - page_off;
        if (chunk > remaining) {
            chunk = remaining;
        }

        if (page_idx < m_page_count && m_pages[page_idx]) {
            string::memcpy(dst, m_pages[page_idx] + page_off, chunk);
        } else {
            string::memset(dst, 0, chunk);
        }

        dst += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    f->set_offset(static_cast<int64_t>(offset + count));
    return static_cast<ssize_t>(count);
}

ssize_t file_node::write(fs::file* f, const void* buf, size_t count) {
    if (!f || !buf) return fs::ERR_BADF;

    sync::irq_lock_guard guard(m_lock);

    int64_t off = f->offset();
    if (f->flags() & fs::O_APPEND) {
        off = static_cast<int64_t>(m_size);
    }
    if (off < 0) return fs::ERR_INVAL;

    size_t offset = static_cast<size_t>(off);
    size_t end_pos = offset + count;
    uint32_t needed_pages = static_cast<uint32_t>((end_pos + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE);

    int32_t err = ensure_capacity(needed_pages);
    if (err != fs::OK) {
        return err;
    }

    const auto* src = static_cast<const uint8_t*>(buf);
    size_t remaining = count;
    size_t pos = offset;

    while (remaining > 0) {
        uint32_t page_idx = static_cast<uint32_t>(pos / pmm::PAGE_SIZE);
        size_t page_off = pos % pmm::PAGE_SIZE;
        size_t chunk = pmm::PAGE_SIZE - page_off;
        if (chunk > remaining) {
            chunk = remaining;
        }

        while (m_page_count <= page_idx) {
            pmm::phys_addr_t phys = pmm::alloc_page();
            if (phys == 0) {
                return fs::ERR_NOMEM;
            }

            auto* virt = static_cast<uint8_t*>(paging::phys_to_virt(phys));
            string::memset(virt, 0, pmm::PAGE_SIZE);
            m_pages[m_page_count] = virt;
            m_page_count++;
        }

        string::memcpy(m_pages[page_idx] + page_off, src, chunk);

        src += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    if (end_pos > m_size) {
        m_size = end_pos;
    }

    mark_modified();
    f->set_offset(static_cast<int64_t>(end_pos));

    return static_cast<ssize_t>(count);
}

int64_t file_node::seek(fs::file* f, int64_t offset, int whence) {
    if (!f) return fs::ERR_BADF;

    sync::irq_lock_guard guard(m_lock);

    int64_t new_off;
    switch (whence) {
        case fs::SEEK_SET:
            new_off = offset;
            break;
        case fs::SEEK_CUR:
            new_off = f->offset() + offset;
            break;
        case fs::SEEK_END:
            new_off = static_cast<int64_t>(m_size) + offset;
            break;
        default:
            return fs::ERR_INVAL;
    }

    if (new_off < 0) {
        return fs::ERR_INVAL;
    }

    f->set_offset(new_off);
    return new_off;
}

int32_t file_node::truncate(size_t size) {
    size_t max_alignable = ~(pmm::PAGE_SIZE - 1);
    if (size > max_alignable) {
        return fs::ERR_INVAL;
    }

    sync::irq_lock_guard guard(m_lock);

    uint32_t needed = static_cast<uint32_t>(
        pmm::page_align_up(size) / pmm::PAGE_SIZE);

    if (needed > m_page_count) {
        int32_t rc = ensure_capacity(needed);
        if (rc != fs::OK) {
            return rc;
        }

        for (uint32_t i = m_page_count; i < needed; i++) {
            pmm::phys_addr_t phys = pmm::alloc_page();
            if (phys == 0) {
                m_size = static_cast<size_t>(i) * pmm::PAGE_SIZE;
                m_page_count = i;
                return fs::ERR_NOMEM;
            }

            m_pages[i] = static_cast<uint8_t*>(paging::phys_to_virt(phys));
            string::memset(m_pages[i], 0, pmm::PAGE_SIZE);
        }
        m_page_count = needed;
    } else if (needed < m_page_count) {
        for (uint32_t i = needed; i < m_page_count; i++) {
            if (m_pages[i]) {
                pmm::phys_addr_t phys =
                    reinterpret_cast<uintptr_t>(m_pages[i]) - g_boot_info.hhdm_offset;
                pmm::free_page(phys);
                m_pages[i] = nullptr;
            }
        }
        m_page_count = needed;
    }

    if (size < m_size && needed > 0) {
        size_t tail_off = size % pmm::PAGE_SIZE;
        if (tail_off != 0 && m_pages[needed - 1]) {
            string::memset(m_pages[needed - 1] + tail_off, 0, pmm::PAGE_SIZE - tail_off);
        }
    }

    m_size = size;
    mark_modified();

    return fs::OK;
}

} // namespace ramfs
