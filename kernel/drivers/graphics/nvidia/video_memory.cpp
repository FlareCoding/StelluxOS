// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/graphics/nvidia/bar0_window.h"
#include "drivers/graphics/nvidia/bar1.h"
#include "drivers/graphics/nvidia/bar1_page_tables.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "hw/barrier.h"
#include "hw/mmio.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// Page table entries written through BAR0's window at a time, 512 bytes, which always fit the 1 MiB it reaches
constexpr uint64_t ENTRIES_PER_WRITE = 64;

bar1_video_memory::bar1_video_memory() {
    m_mappings.init();
    m_lock.init();
}

bar1_video_memory::~bar1_video_memory() {
    free_mappings();
}

int32_t bar1_video_memory::init(const static_configuration& configuration, uint64_t video_memory_size) {
    return m_allocator.init(configuration, video_memory_size);
}

int32_t bar1_video_memory::set_up_bar1(
    registers regs,
    pmm::phys_addr_t bar1_phys,
    uint64_t bar1_bytes,
    uint64_t root_offset,
    uint64_t console_bytes
) {
    uint64_t window_bytes = bar1_window_bytes(bar1_bytes);
    uint64_t tables_bytes = bar1_page_table_bytes(window_bytes);
    vram_range tables_range = {};
    int32_t rc = m_allocator.allocate(tables_bytes, SMALL_PAGE_BYTES, vram_use::general, tables_range);
    if (rc != OK) {
        log::error("nvidia: allocating 0x%lX bytes of video memory for BAR1's tables failed: %d", tables_bytes, rc);
        return rc;
    }

    uint64_t* tables = static_cast<uint64_t*>(heap::ualloc(tables_bytes));
    if (tables == nullptr) {
        log::error("nvidia: allocating 0x%lX bytes for the CPU's copy of BAR1's tables failed", tables_bytes);
        return ERR_NO_MEMORY;
    }

    uint64_t root_entry = write_bar1_page_tables(tables, tables_range.offset, window_bytes, console_bytes);
    if (root_entry == 0) {
        log::error(
            "nvidia: BAR1's tables, 0x%lX bytes at 0x%lX in video memory, end past 128 GiB, the reach of an entry's "
            "address field",
            tables_bytes,
            tables_range.offset
        );
        heap::ufree(tables);
        return ERR_BAD_STATE;
    }

    // BAR0's window reaches 1 MiB from the 64 KiB step a write starts in, so each write copies one 4 KiB table
    uint64_t table_words = SMALL_PAGE_BYTES / sizeof(uint64_t);
    for (uint64_t written = 0; written < tables_bytes; written += SMALL_PAGE_BYTES) {
        uint64_t* table = tables + written / sizeof(uint64_t);
        rc = write_video_memory(regs, tables_range.offset + written, table, table_words);
        if (rc != OK) {
            break;
        }
    }

    heap::ufree(tables);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: BAR1's tables for a window of BAR1's first 0x%lX bytes, mapping the first 0x%lX of them onto the same "
        "offsets of video memory, are written as 0x%lX bytes at 0x%lX in video memory",
        window_bytes,
        console_bytes,
        tables_bytes,
        tables_range.offset
    );

    rc = link_bar1_page_tables(regs, root_offset, root_entry, DEFAULT_TIMEOUT_NS);
    if (rc != OK) {
        return rc;
    }

    record_window(regs, bar1_phys, bar1_bytes, root_offset, tables_range.offset, console_bytes);
    return OK;
}

void bar1_video_memory::open_window(uintptr_t bar1_virt) {
    RUN_ELEVATED(sync::mutex_lock(m_lock));

    m_bar1_virt = bar1_virt;
    m_window_bytes = bar1_window_bytes(m_bar1_bytes);

    RUN_ELEVATED(sync::mutex_unlock(m_lock));
}

int32_t bar1_video_memory::allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) {
    return m_allocator.allocate(size, alignment, use, out);
}

void bar1_video_memory::release(const vram_range& range) {
    RUN_ELEVATED(sync::mutex_lock(m_lock));

    mapping* held = find_mapping(range);
    if (held != nullptr) {
        unmap_locked(held);
    }

    m_allocator.release(range);

    RUN_ELEVATED(sync::mutex_unlock(m_lock));
}

int32_t bar1_video_memory::map_for_cpu(const vram_range& range, cpu_mapping& out) {
    RUN_ELEVATED(sync::mutex_lock(m_lock));

    int32_t rc = map_locked(range, out);

    RUN_ELEVATED(sync::mutex_unlock(m_lock));

    return rc;
}

void bar1_video_memory::unmap_for_cpu(const vram_range& range) {
    RUN_ELEVATED(sync::mutex_lock(m_lock));

    mapping* held = find_mapping(range);
    bool is_mapped = held != nullptr;
    if (is_mapped) {
        unmap_locked(held);
    }

    RUN_ELEVATED(sync::mutex_unlock(m_lock));

    if (!is_mapped) {
        log::error(
            "nvidia: unmapping 0x%lX bytes of video memory at 0x%lX changes nothing, as they are not mapped",
            range.size,
            range.offset
        );
    }
}

void bar1_video_memory::drop_window() {
    RUN_ELEVATED(sync::mutex_lock(m_lock));

    free_mappings();
    m_regs = registers(0);
    m_bar1_virt = 0;
    m_bar1_phys = 0;
    m_bar1_bytes = 0;
    m_window_bytes = 0;
    m_console_bytes = 0;

    RUN_ELEVATED(sync::mutex_unlock(m_lock));
}

void bar1_video_memory::record_window(
    registers regs,
    pmm::phys_addr_t bar1_phys,
    uint64_t bar1_bytes,
    uint64_t root_offset,
    uint64_t tables_offset,
    uint64_t console_bytes
) {
    RUN_ELEVATED(sync::mutex_lock(m_lock));

    m_regs = regs;
    m_bar1_phys = bar1_phys;
    m_bar1_bytes = bar1_bytes;
    m_root_offset = root_offset;
    m_tables_offset = tables_offset;
    m_console_bytes = console_bytes;

    RUN_ELEVATED(sync::mutex_unlock(m_lock));
}

int32_t bar1_video_memory::map_locked(const vram_range& range, cpu_mapping& out) {
    if (!m_allocator.is_handed_out(range)) {
        log::error(
            "nvidia: mapping 0x%lX bytes of video memory at 0x%lX for the CPU was refused, as they are not an "
            "allocation handed out whole",
            range.size,
            range.offset
        );
        return ERR_INVALID_ARG;
    }

    if (find_mapping(range) != nullptr) {
        log::error(
            "nvidia: mapping 0x%lX bytes of video memory at 0x%lX for the CPU was refused, as they are mapped already",
            range.size,
            range.offset
        );
        return ERR_INVALID_ARG;
    }

    uint64_t last_page_offset = range.offset + range.size - SMALL_PAGE_BYTES;
    if (bar1_page_entry(last_page_offset) == 0) {
        log::error(
            "nvidia: mapping 0x%lX bytes of video memory at 0x%lX for the CPU was refused, as they end past 128 GiB, "
            "the reach of a page table entry's address field",
            range.size,
            range.offset
        );
        return ERR_INVALID_ARG;
    }

    uint64_t bar1_offset = 0;
    if (!find_free_run(range.size, bar1_offset)) {
        log::error(
            "nvidia: BAR1's window of 0x%lX bytes has no stretch of 0x%lX unmapped bytes past the console's 0x%lX to "
            "map video memory at 0x%lX for the CPU",
            m_window_bytes,
            range.size,
            m_console_bytes,
            range.offset
        );
        return ERR_NO_MEMORY;
    }

    mapping* added = heap::ualloc_new<mapping>();
    if (added == nullptr) {
        log::error(
            "nvidia: allocating the record of a mapping of 0x%lX bytes of video memory at 0x%lX failed, so they are "
            "not mapped",
            range.size,
            range.offset
        );
        return ERR_NO_MEMORY;
    }

    added->bar1_offset = bar1_offset;
    added->range = range;
    m_mappings.insert_sorted(added, [](const mapping* a, const mapping* b) {
        return a->bar1_offset < b->bar1_offset;
    });

    int32_t rc = update_run(*added, true);
    if (rc != OK) {
        int32_t rewrite_rc = write_run_entries(*added, false);
        log::error(
            "nvidia: mapping 0x%lX bytes of video memory at 0x%lX through BAR1's window at offset 0x%lX failed: %d, "
            "rewriting the entries there sparse returned %d, and those offsets are freed",
            range.size,
            range.offset,
            bar1_offset,
            rc,
            rewrite_rc
        );
        free_mapping(added);
        return rc;
    }

    out = {.virt = m_bar1_virt + bar1_offset, .phys = m_bar1_phys + bar1_offset};
    return OK;
}

void bar1_video_memory::unmap_locked(mapping* held) {
    // The full fence orders the CPU's writes through the mapping before the read, which returns once they are in video
    // memory while the run's entries still translate them. On x86_64 a store fence orders no read after the writes
    barrier::dma_full();
    mmio::read64(m_bar1_virt + held->bar1_offset);

    int32_t rc = update_run(*held, false);
    if (rc != OK) {
        log::error(
            "nvidia: unmapping 0x%lX bytes of video memory at 0x%lX from BAR1's window at offset 0x%lX failed: %d, and "
            "those offsets are freed all the same",
            held->range.size,
            held->range.offset,
            held->bar1_offset,
            rc
        );
    }

    free_mapping(held);
}

bool bar1_video_memory::find_free_run(uint64_t size, uint64_t& bar1_offset_out) {
    uint64_t candidate = m_console_bytes;
    for (const mapping& held : m_mappings) {
        if (held.bar1_offset - candidate >= size) {
            break;
        }

        candidate = held.bar1_offset + held.range.size;
    }

    if (!lies_inside(candidate, size, m_window_bytes)) {
        return false;
    }

    bar1_offset_out = candidate;
    return true;
}

void bar1_video_memory::free_mappings() {
    mapping* left = m_mappings.pop_front();
    while (left != nullptr) {
        heap::ufree_delete(left);
        left = m_mappings.pop_front();
    }
}

void bar1_video_memory::free_mapping(mapping* held) {
    m_mappings.remove(held);
    heap::ufree_delete(held);
}

bar1_video_memory::mapping* bar1_video_memory::find_mapping(const vram_range& range) {
    for (mapping& held : m_mappings) {
        if (held.range.offset == range.offset && held.range.size == range.size) {
            return &held;
        }
    }

    return nullptr;
}

int32_t bar1_video_memory::write_run_entries(const mapping& held, bool maps_range) {
    uint64_t pages = held.range.size / SMALL_PAGE_BYTES;
    uint64_t entries[ENTRIES_PER_WRITE];
    for (uint64_t first = 0; first < pages; first += ENTRIES_PER_WRITE) {
        uint64_t count = pages - first < ENTRIES_PER_WRITE ? pages - first : ENTRIES_PER_WRITE;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t page_offset = held.range.offset + (first + i) * SMALL_PAGE_BYTES;
            entries[i] = maps_range ? bar1_page_entry(page_offset) : bar1_sparse_page_entry();
        }

        uint64_t entries_offset = bar1_page_entry_offset(m_tables_offset, held.bar1_offset + first * SMALL_PAGE_BYTES);
        int32_t rc = write_video_memory(m_regs, entries_offset, entries, count);
        if (rc != OK) {
            return rc;
        }
    }

    return OK;
}

int32_t bar1_video_memory::update_run(const mapping& held, bool maps_range) {
    int32_t rc = write_run_entries(held, maps_range);
    if (rc != OK) {
        return rc;
    }

    uint64_t last_page_offset = held.range.size - SMALL_PAGE_BYTES;
    uint64_t last_entry_offset = bar1_page_entry_offset(m_tables_offset, held.bar1_offset + last_page_offset);
    uint64_t expected = maps_range ? bar1_page_entry(held.range.offset + last_page_offset) : bar1_sparse_page_entry();
    uint64_t found = read_video_memory_u64(m_regs, last_entry_offset);
    if (found != expected) {
        log::error(
            "nvidia: BAR1's page table entry at 0x%lX in video memory reads back as 0x%lX after 0x%lX was written",
            last_entry_offset,
            found,
            expected
        );
        return ERR_BAD_STATE;
    }

    rc = invalidate_bar1_tlb(m_regs, m_root_offset, m_invalidate_timeout_ns);
    if (rc != OK) {
        return rc;
    }

    // A read through the write-combining mapping could otherwise happen before the read that saw the invalidate finish
    barrier::dma_read();
    return OK;
}

} // namespace drivers::nvidia
