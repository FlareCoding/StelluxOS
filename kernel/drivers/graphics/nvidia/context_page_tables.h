#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CONTEXT_PAGE_TABLES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CONTEXT_PAGE_TABLES_H

#include "common/list.h"
#include "common/types.h"
#include "drivers/graphics/nvidia/failed_mark.h"
#include "drivers/graphics/nvidia/page_tables.h"
#include "drivers/graphics/nvidia/video_memory.h"

namespace drivers::nvidia {

// Pages to map, at an offset into video memory or a physical address of system memory
struct memory_run {
    uint64_t address; // a multiple of 4 KiB
    uint64_t bytes;   // a multiple of 4 KiB, not 0
};

// The directories from the root down to directory 1: the root, directory 2 and directory 1
constexpr uint32_t DIRECTORIES_DOWN_TO_DIRECTORY_1 = 3;

/**
 * A GPU context's page tables. A GPU context is one GPU address space and the
 * queues that run work in it, and the card translates its addresses through
 * these tables. Each table takes 4 KiB of video memory handed out as
 * `vram_use::general` and a copy on the unprivileged heap: `create` adds the
 * root page directory, `map` each other table when a mapping first needs it,
 * and every table is kept until `destroy`. An address no mapping covers
 * faults. Adding a table fails with `ERR_NO_MEMORY` when either memory has no
 * room for it, or `ERR_BAD_STATE` when it lies past `ENTRY_REACH_BYTES`.
 *
 * The caller must make one call at a time. Every call but `directory_offset`
 * can block in `video_memory`'s calls, so none may run in an interrupt handler
 * or under a spinlock.
 *
 * An update (a `video_memory::update_context_tables` call) that fails sets the
 * GPU's failed mark, as the card may then translate through entries the object
 * no longer knows. From then on the tables stay in video memory, and `create`,
 * `map`, `unmap` and a `destroy` of existing tables return `ERR_GPU_FAILED`.
 * Every other error is logged except no room in video memory for a table,
 * which is left to the caller.
 */
class context_page_tables : private table_writes {
public:
    // An object with no tables, using `memory` and `failed`, which must outlive it
    context_page_tables(video_memory& memory, failed_mark& failed);

    // Frees the copies and leaves in video memory any table `destroy` did not release
    ~context_page_tables() override;

    context_page_tables(const context_page_tables&) = delete;
    context_page_tables& operator=(const context_page_tables&) = delete;

    /**
     * @brief Add the root page directory with every entry invalid, which
     * `map`, `unmap` and `destroy` need first.
     * @return `OK`, `ERR_BAD_STATE` while the tables exist, an error of adding
     * a table, `ERR_GPU_FAILED` once the mark is set, or the update's error.
     */
    int32_t create();

    // Where the root page directory lies in video memory, or 0 while the object holds none
    uint64_t directory_offset() const;

    /**
     * @brief Add and write the directories down to directory 1 translating
     * `gpu_address`, with no invalidate, and give the video memory offsets of
     * the root, directory 2 and directory 1, for a firmware that adds the
     * tables below.
     * @return `OK`, `ERR_BAD_STATE` before `create`, an error of adding a
     * table, with none added, `ERR_GPU_FAILED` once the mark is set, or the
     * update's error.
     */
    int32_t add_directories(uint64_t gpu_address, uint64_t (&offsets)[DIRECTORIES_DOWN_TO_DIRECTORY_1]);

    /**
     * @brief Map the context's addresses from `gpu_address` on onto `runs` in
     * order as `mapping` says, adding the tables they need, so the card
     * translates them once the call returns `OK`.
     * @return `OK`, `ERR_INVALID_ARG` when there are no runs, a run is empty,
     * not whole pages or has a page `page_entry` refuses, or the addresses are
     * not whole pages below `ADDRESS_SPACE_BYTES`, `ERR_BAD_STATE` before
     * `create`, an error of adding a table, with none added, `ERR_GPU_FAILED`
     * once the mark is set, or the update's error.
     */
    int32_t map(uint64_t gpu_address, const memory_run* runs, size_t run_count, const page_mapping& mapping);

    /**
     * @brief Make the `size` bytes of the context's addresses at `gpu_address`
     * invalid, so once the call returns `OK` no access reaches the pages they
     * mapped. It allocates nothing and keeps every table.
     * @return `OK`, `ERR_INVALID_ARG` when `size` is 0 or the addresses are not
     * whole pages below `ADDRESS_SPACE_BYTES`, `ERR_BAD_STATE` before `create`,
     * `ERR_GPU_FAILED` once the mark is set, or the update's error.
     */
    int32_t unmap(uint64_t gpu_address, uint64_t size);

    /**
     * @brief Invalidate the context's cached translations, then release every
     * table, leaving the object as before `create`. No work may still run in
     * the context, since the tables' memory can be handed out again.
     * @return `OK`, `ERR_BAD_STATE` before `create`, `ERR_GPU_FAILED` once the
     * mark is set, or the update's error. On the last two the tables stay in
     * video memory for good and only their copies are freed.
     */
    int32_t destroy();

private:
    // A table in video memory and the copy of its entries, which matches it between calls until an update fails
    struct table {
        vram_range range;
        page_level level;
        table*     parent;
        uint32_t   parent_word; // where the parent's entry naming it starts, in 8-byte words
        table**    children;    // the tables a directory's entries name, by word, or null in a page table
        bool       is_new;      // no entry names it in video memory, so it is written before any entry that does
        bool       is_dirty;
        uint32_t   dirty_first; // the words the next update writes
        uint32_t   dirty_end;
        list::node dirty_link;
        uint64_t   entries[TABLE_BYTES / sizeof(uint64_t)];
    };

    // Hands out each dirty table's dirty words as one run, new tables first
    bool next(entry_run& out) override;

    // The page table translating `gpu_address`, or null when it or a directory above it is missing
    table* find_page_table(uint64_t gpu_address) const;

    // Adds whichever of the directories and the page table translating `gpu_address` are missing
    int32_t add_page_table(uint64_t gpu_address);

    // Allocates a table of `level` with every entry invalid and makes `parent`'s entry for `gpu_address` name it, with
    // `parent` null for the root
    int32_t add_table(table* parent, page_level level, uint64_t gpu_address, table*& out);

    // Marks the words of `changed` from `first` to before `end` for the next update
    void mark_dirty(table* changed, uint32_t first, uint32_t end);

    // Unlinks and releases the tables no update wrote, after `add_page_table` failed
    void remove_new_tables();

    // Writes the dirty words, setting the failed mark when the update fails
    int32_t write_dirty_tables(context_tlbs tlbs);

    // Frees the copies of `freed` and of every table below it, releasing their memory when `releases_memory` is set
    void free_tables(table* freed, bool releases_memory);

    video_memory&                         m_memory;
    failed_mark&                          m_failed;
    table*                                m_root = nullptr;
    list::head<table, &table::dirty_link> m_dirty; // new tables at the front, the others at the back
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CONTEXT_PAGE_TABLES_H
