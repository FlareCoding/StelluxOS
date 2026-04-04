#include "fs/ramfs/ramfs.h"
#include "fs/socket_node.h"
#include "fs/fs.h"
#include "common/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/vma.h"
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
    : fs::node(fs::node_type::directory, fs, name)
    , m_child_count(0) {
    m_children.init();
}

dir_node::~dir_node() {
    // Truly iterative destruction: move children into a worklist,
    // then process the worklist in a flat loop. If a child is itself
    // a dir_node, steal its children into the worklist before destroying
    // it, so the child's destructor sees an empty child list and does
    // not recurse. This bounds stack depth regardless of tree depth.
    list::head<fs::node, &fs::node::m_child_link> worklist;
    worklist.init();

    while (!m_children.empty()) {
        fs::node* child = m_children.pop_front();
        child->set_parent(nullptr);
        worklist.push_back(child);
    }
    m_child_count = 0;

    while (!worklist.empty()) {
        fs::node* n = worklist.pop_front();
        if (n->type() == fs::node_type::directory) {
            auto* dn = static_cast<dir_node*>(n);
            while (!dn->m_children.empty()) {
                fs::node* grandchild = dn->m_children.pop_front();
                grandchild->set_parent(nullptr);
                worklist.push_back(grandchild);
            }
            dn->m_child_count = 0;
        }
        if (n->release()) {
            fs::node::ref_destroy(n);
        }
    }
}

fs::node* dir_node::find_child(const char* name, size_t len) {
    for (auto& child : m_children) {
        size_t child_len = string::strlen(child.name());
        if (child_len == len && string::strncmp(child.name(), name, len) == 0) {
            return &child;
        }
    }
    return nullptr;
}

int32_t dir_node::lookup(const char* name, size_t len, fs::node** out) {
    if (!name || !out) return fs::ERR_INVAL;

    sync::irq_lock_guard guard(m_lock);
    fs::node* child = find_child(name, len);
    if (!child) return fs::ERR_NOENT;

    child->add_ref();
    *out = child;
    return fs::OK;
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

    child->set_parent(this);
    m_children.push_back(child);
    m_child_count++;

    child->add_ref();
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

    child->set_parent(this);
    m_children.push_back(child);
    m_child_count++;

    child->add_ref();
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

    child->set_parent(this);
    m_children.push_back(child);
    m_child_count++;

    child->add_ref();
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

    m_children.remove(child);
    m_child_count--;
    child->set_parent(nullptr);

    if (child->release()) {
        fs::node::ref_destroy(child);
    }
    return fs::OK;
}

int32_t dir_node::rmdir(const char* name, size_t len) {
    if (!name || len == 0) return fs::ERR_INVAL;

    sync::irq_lock_guard guard(m_lock);

    fs::node* child = find_child(name, len);
    if (!child) {
        return fs::ERR_NOENT;
    }
    if (child->type() != fs::node_type::directory) {
        return fs::ERR_NOTDIR;
    }

    auto* child_dir = static_cast<dir_node*>(child);
    if (child_dir->m_child_count > 0) {
        return fs::ERR_NOTEMPTY;
    }

    m_children.remove(child);
    m_child_count--;
    child->set_parent(nullptr);

    if (child->release()) {
        fs::node::ref_destroy(child);
    }
    return fs::OK;
}

ssize_t dir_node::readdir(fs::file* f, fs::dirent* entries, size_t count) {
    if (!f || !entries) return fs::ERR_BADF;
    if (count == 0) return 0;

    sync::irq_lock_guard guard(m_lock);

    size_t idx = static_cast<size_t>(f->offset());
    size_t written = 0;

    size_t cur_idx = 0;
    for (auto& child : m_children) {
        if (written >= count) {
            break;
        }
        if (cur_idx >= idx) {
            size_t name_len = string::strlen(child.name());
            if (name_len > fs::NAME_MAX) {
                name_len = fs::NAME_MAX;
            }
            string::memcpy(entries[written].name, child.name(), name_len);
            entries[written].name[name_len] = '\0';
            entries[written].type = child.type();
            written++;
        }
        cur_idx++;
    }

    f->set_offset(static_cast<int64_t>(idx + written));
    return static_cast<ssize_t>(written);
}

int32_t dir_node::getattr(fs::vattr* attr) {
    if (!attr) return fs::ERR_INVAL;
    attr->type = fs::node_type::directory;
    attr->size = m_child_count;
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

int32_t file_node::getattr(fs::vattr* attr) {
    if (!attr) return fs::ERR_INVAL;
    attr->type = fs::node_type::regular;
    attr->size = m_size;
    return fs::OK;
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
    return fs::OK;
}

int32_t file_node::mmap(fs::file* f, mm::mm_context* mm_ctx, uintptr_t addr,
                        size_t length, uint32_t prot, uint32_t map_flags,
                        uint64_t offset, uintptr_t* out_addr) {
    (void)f;
    if (!mm_ctx || !out_addr || length == 0) return fs::ERR_INVAL;

    size_t aligned_len = pmm::page_align_up(length);
    if (offset % pmm::PAGE_SIZE != 0) return fs::ERR_INVAL;
    if (offset + aligned_len > pmm::page_align_up(m_size)) return fs::ERR_INVAL;

    bool fixed = (map_flags & mm::MM_MAP_FIXED) != 0;

    sync::mutex_lock(mm_ctx->lock);

    uintptr_t start;
    if (fixed) {
        if (addr % pmm::PAGE_SIZE != 0) {
            sync::mutex_unlock(mm_ctx->lock);
            return fs::ERR_INVAL;
        }
        start = addr;
    } else {
        start = mm::vma_find_gap_topdown_locked(mm_ctx, aligned_len);
        if (start == 0) {
            sync::mutex_unlock(mm_ctx->lock);
            return fs::ERR_NOMEM;
        }
    }

    paging::page_flags_t page_flags = paging::PAGE_USER | paging::PAGE_READ;
    if (prot & mm::MM_PROT_WRITE) page_flags |= paging::PAGE_WRITE;
    if (prot & mm::MM_PROT_EXEC) page_flags |= paging::PAGE_EXEC;

    size_t pages = aligned_len / pmm::PAGE_SIZE;
    size_t page_offset = offset / pmm::PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        uint32_t pidx = static_cast<uint32_t>(page_offset + i);
        if (pidx >= m_page_count || !m_pages[pidx]) {
            // Unmap any pages we already mapped
            for (size_t j = 0; j < i; j++) {
                paging::unmap_page(start + j * pmm::PAGE_SIZE, mm_ctx->pt_root);
            }
            sync::mutex_unlock(mm_ctx->lock);
            return fs::ERR_IO;
        }

        pmm::phys_addr_t phys =
            reinterpret_cast<uintptr_t>(m_pages[pidx]) - g_boot_info.hhdm_offset;
        uintptr_t vaddr = start + i * pmm::PAGE_SIZE;

        if (paging::map_page(vaddr, phys, page_flags, mm_ctx->pt_root) != paging::OK) {
            for (size_t j = 0; j < i; j++) {
                paging::unmap_page(start + j * pmm::PAGE_SIZE, mm_ctx->pt_root);
            }
            sync::mutex_unlock(mm_ctx->lock);
            return fs::ERR_IO;
        }
    }

    // VMA_FLAG_SHARED prevents page freeing on unmap (ramfs owns the pages)
    auto* node = heap::kalloc_new<mm::vma>();
    if (!node) {
        for (size_t i = 0; i < pages; i++) {
            paging::unmap_page(start + i * pmm::PAGE_SIZE, mm_ctx->pt_root);
        }
        sync::mutex_unlock(mm_ctx->lock);
        return fs::ERR_NOMEM;
    }
    node->start = start;
    node->end = start + aligned_len;
    node->prot = prot;
    node->flags = mm::VMA_FLAG_SHARED;
    node->addr_link = {};
    node->backing_offset = 0;

    if (!mm::vma_insert_locked(mm_ctx, node)) {
        for (size_t i = 0; i < pages; i++) {
            paging::unmap_page(start + i * pmm::PAGE_SIZE, mm_ctx->pt_root);
        }
        heap::kfree_delete(node);
        sync::mutex_unlock(mm_ctx->lock);
        return fs::ERR_NOMEM;
    }

    sync::mutex_unlock(mm_ctx->lock);
    *out_addr = start;
    return 0;
}

} // namespace ramfs
