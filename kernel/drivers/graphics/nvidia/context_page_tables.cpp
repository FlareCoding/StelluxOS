#include "drivers/graphics/nvidia/context_page_tables.h"
#include "common/logging.h"
#include "mm/heap.h"

namespace drivers::nvidia {

constexpr uint32_t TABLE_WORDS    = TABLE_BYTES / sizeof(uint64_t);
constexpr uint64_t CHILDREN_BYTES = TABLE_WORDS * sizeof(void*);

static page_level level_below(page_level level) {
    switch (level) {
        case page_level::root:
            return page_level::directory_2;
        case page_level::directory_2:
            return page_level::directory_1;
        case page_level::directory_1:
            return page_level::directory_0;
        case page_level::directory_0:
        case page_level::page_table:
            break;
    }

    return page_level::page_table;
}

static uint32_t entry_word(page_level level, uint64_t gpu_address) {
    return static_cast<uint32_t>(table_entry_offset(level, gpu_address) / sizeof(uint64_t));
}

static uint32_t entry_words(page_level level) {
    return static_cast<uint32_t>(entry_bytes(level) / sizeof(uint64_t));
}

static uint64_t next_page_table_start(uint64_t gpu_address) {
    return (gpu_address | (entry_span(page_level::directory_0) - 1)) + 1;
}

context_page_tables::context_page_tables(video_memory& memory, failed_mark& failed)
    : m_memory(memory), m_failed(failed) {
    m_dirty.init();
}

context_page_tables::~context_page_tables() {
    if (m_root != nullptr) {
        free_tables(m_root, false);
    }
}

int32_t context_page_tables::create() {
    if (m_failed.is_set()) {
        return ERR_GPU_FAILED;
    }

    if (m_root != nullptr) {
        log::error("nvidia: a GPU context's page tables are not created again, as they exist");
        return ERR_BAD_STATE;
    }

    int32_t rc = add_table(nullptr, page_level::root, 0, m_root);
    if (rc != OK) {
        return rc;
    }

    return write_dirty_tables(context_tlbs::kept);
}

uint64_t context_page_tables::directory_offset() const {
    return m_root != nullptr ? m_root->range.offset : 0;
}

int32_t context_page_tables::map(
    uint64_t gpu_address,
    const memory_run* runs,
    size_t run_count,
    const page_mapping& mapping
) {
    if (m_failed.is_set()) {
        return ERR_GPU_FAILED;
    }

    if (m_root == nullptr) {
        log::error("nvidia: a GPU context's addresses are not mapped, as its page tables are not created");
        return ERR_BAD_STATE;
    }

    uint64_t size = 0;
    for (size_t i = 0; i < run_count; i++) {
        const memory_run& run = runs[i];
        uint64_t last_page = run.address + run.bytes - SMALL_PAGE_BYTES;
        bool is_whole_pages = run.bytes != 0 && run.bytes % SMALL_PAGE_BYTES == 0;
        bool fits_addresses = is_whole_pages && run.bytes <= ADDRESS_SPACE_BYTES - size;
        bool names_pages = page_entry(run.address, mapping) != 0 && page_entry(last_page, mapping) != 0;
        if (!fits_addresses || !names_pages) {
            log::error(
                "nvidia: a mapping at 0x%lX of a GPU context's addresses is refused, as its run %lu, 0x%lX bytes at "
                "0x%lX, is empty, not whole pages or past the context's addresses, or names memory or a kind no page "
                "table entry can",
                gpu_address,
                i,
                run.bytes,
                run.address
            );
            return ERR_INVALID_ARG;
        }

        size += run.bytes;
    }

    bool is_whole_pages = gpu_address % SMALL_PAGE_BYTES == 0 && size != 0;
    if (!is_whole_pages || !lies_inside(gpu_address, size, ADDRESS_SPACE_BYTES)) {
        log::error(
            "nvidia: 0x%lX bytes at 0x%lX of a GPU context's addresses are not mapped, as they are not whole pages "
            "inside the addresses",
            size,
            gpu_address
        );
        return ERR_INVALID_ARG;
    }

    uint64_t end = gpu_address + size;
    for (uint64_t covered = gpu_address; covered < end; covered = next_page_table_start(covered)) {
        int32_t rc = add_page_table(covered);
        if (rc != OK) {
            remove_new_tables();
            return rc;
        }
    }

    uint64_t page_address = gpu_address;
    for (size_t i = 0; i < run_count; i++) {
        for (uint64_t done = 0; done < runs[i].bytes; done += SMALL_PAGE_BYTES) {
            table* entries_table = find_page_table(page_address);
            uint32_t word = entry_word(page_level::page_table, page_address);
            entries_table->entries[word] = page_entry(runs[i].address + done, mapping);
            mark_dirty(entries_table, word, word + 1);
            page_address += SMALL_PAGE_BYTES;
        }
    }

    return write_dirty_tables(context_tlbs::invalidated);
}

int32_t context_page_tables::unmap(uint64_t gpu_address, uint64_t size) {
    if (m_failed.is_set()) {
        return ERR_GPU_FAILED;
    }

    if (m_root == nullptr) {
        log::error("nvidia: a GPU context's addresses are not unmapped, as its page tables are not created");
        return ERR_BAD_STATE;
    }

    bool is_whole_pages = gpu_address % SMALL_PAGE_BYTES == 0 && size % SMALL_PAGE_BYTES == 0 && size != 0;
    if (!is_whole_pages || !lies_inside(gpu_address, size, ADDRESS_SPACE_BYTES)) {
        log::error(
            "nvidia: 0x%lX bytes at 0x%lX of a GPU context's addresses are not unmapped, as they are not whole "
            "pages inside the addresses",
            size,
            gpu_address
        );
        return ERR_INVALID_ARG;
    }

    uint64_t end = gpu_address + size;
    for (uint64_t first = gpu_address; first < end;) {
        uint64_t next_start = next_page_table_start(first);
        uint64_t table_end = next_start < end ? next_start : end;
        table* entries_table = find_page_table(first);
        if (entries_table != nullptr) {
            uint32_t first_word = entry_word(page_level::page_table, first);
            uint32_t end_word = first_word + static_cast<uint32_t>((table_end - first) / SMALL_PAGE_BYTES);
            for (uint32_t word = first_word; word < end_word; word++) {
                entries_table->entries[word] = 0;
            }

            mark_dirty(entries_table, first_word, end_word);
        }

        first = table_end;
    }

    if (m_dirty.empty()) {
        return OK;
    }

    return write_dirty_tables(context_tlbs::invalidated);
}

int32_t context_page_tables::destroy() {
    if (m_root == nullptr) {
        log::error("nvidia: a GPU context's page tables are not destroyed, as they are not created");
        return ERR_BAD_STATE;
    }

    int32_t rc = m_failed.is_set() ? ERR_GPU_FAILED : write_dirty_tables(context_tlbs::invalidated);
    free_tables(m_root, rc == OK);
    m_root = nullptr;
    return rc;
}

bool context_page_tables::next(entry_run& out) {
    table* written = m_dirty.pop_front();
    if (written == nullptr) {
        return false;
    }

    out = {
        .vram_offset = written->range.offset + written->dirty_first * sizeof(uint64_t),
        .entries = written->entries + written->dirty_first,
        .count = written->dirty_end - written->dirty_first,
    };

    written->is_new = false;
    written->is_dirty = false;
    return true;
}

context_page_tables::table* context_page_tables::find_page_table(uint64_t gpu_address) const {
    table* found = m_root;
    while (found != nullptr && found->level != page_level::page_table) {
        found = found->children[entry_word(found->level, gpu_address)];
    }

    return found;
}

int32_t context_page_tables::add_page_table(uint64_t gpu_address) {
    table* parent = m_root;
    while (parent->level != page_level::page_table) {
        table* child = parent->children[entry_word(parent->level, gpu_address)];
        if (child == nullptr) {
            int32_t rc = add_table(parent, level_below(parent->level), gpu_address, child);
            if (rc != OK) {
                return rc;
            }
        }

        parent = child;
    }

    return OK;
}

int32_t context_page_tables::add_table(table* parent, page_level level, uint64_t gpu_address, table*& out) {
    vram_range range = {};
    int32_t rc = m_memory.allocate(TABLE_BYTES, TABLE_BYTES, vram_use::general, range);
    if (rc != OK) {
        log::error(
            "nvidia: a GPU context's table is not added, as allocating its 4 KiB of video memory failed: %d",
            rc
        );
        return rc;
    }

    if (directory_entry(range.offset) == 0) {
        log::error(
            "nvidia: a GPU context's table at 0x%lX in video memory is not added, as no directory entry can name it",
            range.offset
        );
        m_memory.release(range);
        return ERR_BAD_STATE;
    }

    table* added = heap::ualloc_new<table>();
    table** children = nullptr;
    if (added != nullptr && level != page_level::page_table) {
        children = static_cast<table**>(heap::uzalloc(CHILDREN_BYTES));
    }

    if (added == nullptr || (level != page_level::page_table && children == nullptr)) {
        log::error("nvidia: a GPU context's table is not added, as the unprivileged heap has no room for its copy");
        heap::ufree_delete(added);
        m_memory.release(range);
        return ERR_NO_MEMORY;
    }

    added->range = range;
    added->level = level;
    added->parent = parent;
    added->children = children;

    added->is_new = true;
    added->is_dirty = true;
    added->dirty_first = 0;
    added->dirty_end = TABLE_WORDS;
    m_dirty.push_front(added);

    if (parent != nullptr) {
        uint32_t word = entry_word(parent->level, gpu_address);
        added->parent_word = word;
        parent->children[word] = added;
        write_directory_entry(parent->entries, parent->level, gpu_address, range.offset);
        mark_dirty(parent, word, word + entry_words(parent->level));
    }

    out = added;
    return OK;
}

void context_page_tables::mark_dirty(table* changed, uint32_t first, uint32_t end) {
    if (!changed->is_dirty) {
        changed->is_dirty = true;
        changed->dirty_first = first;
        changed->dirty_end = end;
        m_dirty.push_back(changed);
        return;
    }

    changed->dirty_first = first < changed->dirty_first ? first : changed->dirty_first;
    changed->dirty_end = end > changed->dirty_end ? end : changed->dirty_end;
}

void context_page_tables::remove_new_tables() {
    while (!m_dirty.empty() && m_dirty.front()->is_new) {
        table* removed = m_dirty.pop_front();
        table* parent = removed->parent;
        uint32_t end_word = removed->parent_word + entry_words(parent->level);
        for (uint32_t word = removed->parent_word; word < end_word; word++) {
            parent->entries[word] = 0;
        }

        parent->children[removed->parent_word] = nullptr;
        free_tables(removed, true);
    }

    for (table* kept = m_dirty.pop_front(); kept != nullptr; kept = m_dirty.pop_front()) {
        kept->is_dirty = false;
    }
}

int32_t context_page_tables::write_dirty_tables(context_tlbs tlbs) {
    int32_t rc = m_memory.update_context_tables(m_root->range.offset, *this, tlbs);

    for (table* left = m_dirty.pop_front(); left != nullptr; left = m_dirty.pop_front()) {
        left->is_new = false;
        left->is_dirty = false;
    }

    if (rc != OK) {
        m_failed.set();
        log::error("nvidia: the GPU is marked failed, as an update of a GPU context's page tables returned %d", rc);
    }

    return rc;
}

void context_page_tables::free_tables(table* freed, bool releases_memory) {
    if (freed->children != nullptr) {
        for (uint32_t word = 0; word < TABLE_WORDS; word++) {
            if (freed->children[word] != nullptr) {
                free_tables(freed->children[word], releases_memory);
            }
        }

        heap::ufree(freed->children);
    }

    if (releases_memory) {
        m_memory.release(freed->range);
    }

    heap::ufree_delete(freed);
}

} // namespace drivers::nvidia
