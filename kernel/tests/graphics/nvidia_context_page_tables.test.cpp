// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/context_page_tables.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_context_page_tables);

using drivers::nvidia::ADDRESS_SPACE_BYTES;
using drivers::nvidia::ENTRY_REACH_BYTES;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_GPU_FAILED;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::OK;
using drivers::nvidia::TABLE_BYTES;
using drivers::nvidia::context_page_tables;
using drivers::nvidia::context_tlbs;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::directory_entry;
using drivers::nvidia::entry_run;
using drivers::nvidia::failed_mark;
using drivers::nvidia::memory_report;
using drivers::nvidia::memory_run;
using drivers::nvidia::page_aperture;
using drivers::nvidia::page_entry;
using drivers::nvidia::page_mapping;
using drivers::nvidia::table_writes;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;

static constexpr uint64_t PAGE_BYTES  = 0x1000;
static constexpr uint64_t TABLE_WORDS = 512;

// Where the fake hands out tables, one after the next and never twice, and how many it holds
static constexpr uint64_t FIRST_TABLE_OFFSET = 0x400000;
static constexpr uint64_t FAKE_TABLES        = 16;

// A value no entry takes, filling the fake's tables before the first update
static constexpr uint64_t UNWRITTEN_WORD = 0xA5A5A5A5A5A5A5A5;

// How many of one update's runs the fake records
static constexpr uint32_t RECORDED_RUNS = 8;

// An address whose index is 1 in the root directory, 3 at level 2, 5 at level 1, 7 at level 0 and 9 in its page table
static constexpr uint64_t INDEXED_ADDRESS = 1ULL << 47 | 3ULL << 38 | 5ULL << 29 | 7ULL << 21 | 9ULL << 12;

// Video memory a test maps, and kind 6, NVIDIA's kind for generic memory
static constexpr uint64_t MAPPED_VRAM = 0x1235000;
static constexpr page_mapping GENERIC_MEMORY = {.kind = 6};

// A video memory whose context tables are plain memory, counting updates and invalidates, recording the last update's
// runs and answering each with `update_result`. It refuses with `ERR_BAD_STATE` a run that is not whole entries of one
// table it handed out and holds, and `refused_calls` counts that refusal and each release of a table it does not hold
class table_video_memory : public video_memory {
public:
    uint64_t* words = static_cast<uint64_t*>(heap::ualloc(FAKE_TABLES * TABLE_BYTES));
    uint64_t  next_offset = FIRST_TABLE_OFFSET;
    uint32_t  allocations_left = FAKE_TABLES;
    uint32_t  allocations = 0;
    uint32_t  releases = 0;
    bool      is_released[FAKE_TABLES] = {};
    uint32_t  refused_calls = 0;
    uint32_t  updates = 0;
    uint32_t  invalidates = 0;
    uint32_t  releases_at_last_update = 0;
    uint64_t  invalidated_directory = 0;
    uint32_t  run_count = 0;
    uint64_t  run_offsets[RECORDED_RUNS] = {};
    int32_t   update_result = OK;

    table_video_memory() {
        for (uint64_t i = 0; words != nullptr && i < FAKE_TABLES * TABLE_WORDS; i++) {
            words[i] = UNWRITTEN_WORD;
        }
    }

    ~table_video_memory() override { heap::ufree(words); }

    void report(memory_report& out) override {
        out = {};
    }

    int32_t allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) override {
        bool is_table = size == TABLE_BYTES && alignment == TABLE_BYTES && use == vram_use::general;
        bool has_room = allocations_left != 0 && allocations < FAKE_TABLES;
        if (!is_table || !has_room) {
            return ERR_NO_MEMORY;
        }

        allocations_left--;
        allocations++;
        out = {.offset = next_offset, .size = size};
        next_offset += size;
        return OK;
    }

    void release(const vram_range& range) override {
        if (range.size != TABLE_BYTES || range.offset % TABLE_BYTES != 0 || !holds_table_at(range.offset)) {
            refused_calls++;
            return;
        }

        is_released[(range.offset - FIRST_TABLE_OFFSET) / TABLE_BYTES] = true;
        releases++;
    }

    int32_t map_for_cpu(const vram_range&, cpu_mapping&) override { return ERR_INVALID_ARG; }

    void unmap_for_cpu(const vram_range&) override {}

    int32_t update_context_tables(uint64_t directory_offset, table_writes& writes, context_tlbs tlbs) override {
        updates++;
        releases_at_last_update = releases;
        run_count = 0;
        entry_run run = {};
        while (writes.next(run)) {
            if (!holds_run(run)) {
                refused_calls++;
                return ERR_BAD_STATE;
            }

            if (run_count < RECORDED_RUNS) {
                run_offsets[run_count] = run.vram_offset;
            }

            run_count++;
            string::memcpy(word_at(run.vram_offset), run.entries, run.count * sizeof(uint64_t));
        }

        if (tlbs == context_tlbs::invalidated) {
            invalidates++;
            invalidated_directory = directory_offset;
        }

        return update_result;
    }

    // Entry `index` of the table at `table_offset` as the updates wrote it
    uint64_t entry(uint64_t table_offset, uint64_t index) const { return *word_at(table_offset + index * 8); }

    // How many words of the table at `table_offset` hold `value`
    uint64_t words_holding(uint64_t table_offset, uint64_t value) const {
        uint64_t holding = 0;
        for (uint64_t i = 0; i < TABLE_WORDS; i++) {
            holding += entry(table_offset, i) == value;
        }

        return holding;
    }

private:
    // Whether the table holding `vram_offset` is one the fake handed out and has not had released
    bool holds_table_at(uint64_t vram_offset) const {
        uint64_t table = (vram_offset - FIRST_TABLE_OFFSET) / TABLE_BYTES;
        return vram_offset >= FIRST_TABLE_OFFSET && table < allocations && !is_released[table];
    }

    // Whether `run` is whole entries, at least one, inside one table the fake holds
    bool holds_run(const entry_run& run) const {
        uint64_t words_left = (TABLE_BYTES - run.vram_offset % TABLE_BYTES) / sizeof(uint64_t);
        bool is_whole_entries = run.entries != nullptr && run.count != 0 && run.vram_offset % sizeof(uint64_t) == 0;
        return is_whole_entries && run.count <= words_left && holds_table_at(run.vram_offset);
    }

    // The fake's word at `vram_offset`, which must lie in one of its tables
    uint64_t* word_at(uint64_t vram_offset) const { return words + (vram_offset - FIRST_TABLE_OFFSET) / 8; }
};

// Where the fake places the root page directory and the 4 KiB tables after it, in the order a mapping adds them
static constexpr uint64_t table_offset(uint64_t order) {
    return FIRST_TABLE_OFFSET + order * TABLE_BYTES;
}

TEST(nvidia_context_page_tables, create_writes_a_root_directory_of_invalid_entries_without_an_invalidate) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);

    memory_run run = {.address = MAPPED_VRAM, .bytes = PAGE_BYTES};
    EXPECT_EQ(tables.map(0, &run, 1, GENERIC_MEMORY), ERR_BAD_STATE);
    EXPECT_EQ(tables.unmap(0, PAGE_BYTES), ERR_BAD_STATE);
    EXPECT_EQ(tables.destroy(), ERR_BAD_STATE);
    EXPECT_EQ(tables.directory_offset(), 0u);

    ASSERT_EQ(tables.create(), OK);
    EXPECT_EQ(tables.directory_offset(), table_offset(0));
    EXPECT_EQ(memory.updates, 1u);
    EXPECT_EQ(memory.invalidates, 0u);
    EXPECT_EQ(memory.run_count, 1u);
    EXPECT_EQ(memory.words_holding(table_offset(0), 0), TABLE_WORDS);

    EXPECT_EQ(tables.create(), ERR_BAD_STATE);
    EXPECT_EQ(memory.allocations, 1u);
    EXPECT_EQ(tables.destroy(), OK);
}

TEST(nvidia_context_page_tables, a_first_mapping_writes_its_four_tables_before_the_root_entry_naming_them) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);
    ASSERT_EQ(tables.create(), OK);

    memory_run run = {.address = MAPPED_VRAM, .bytes = 2 * PAGE_BYTES};
    ASSERT_EQ(tables.map(INDEXED_ADDRESS, &run, 1, GENERIC_MEMORY), OK);
    EXPECT_EQ(memory.allocations, 5u);
    EXPECT_EQ(memory.invalidates, 1u);
    EXPECT_EQ(memory.invalidated_directory, table_offset(0));

    // The page table, then directories 0, 1 and 2, each written whole, then the root's entry 1
    ASSERT_EQ(memory.run_count, 5u);
    EXPECT_EQ(memory.run_offsets[0], table_offset(4));
    EXPECT_EQ(memory.run_offsets[1], table_offset(3));
    EXPECT_EQ(memory.run_offsets[2], table_offset(2));
    EXPECT_EQ(memory.run_offsets[3], table_offset(1));
    EXPECT_EQ(memory.run_offsets[4], table_offset(0) + 8);

    EXPECT_EQ(memory.entry(table_offset(0), 1), directory_entry(table_offset(1)));
    EXPECT_EQ(memory.entry(table_offset(1), 3), directory_entry(table_offset(2)));
    EXPECT_EQ(memory.entry(table_offset(2), 5), directory_entry(table_offset(3)));
    EXPECT_EQ(memory.entry(table_offset(3), 14), 0u);
    EXPECT_EQ(memory.entry(table_offset(3), 15), directory_entry(table_offset(4)));

    EXPECT_EQ(memory.words_holding(table_offset(1), 0), TABLE_WORDS - 1);
    EXPECT_EQ(memory.words_holding(table_offset(2), 0), TABLE_WORDS - 1);
    EXPECT_EQ(memory.words_holding(table_offset(3), 0), TABLE_WORDS - 1);

    EXPECT_EQ(memory.entry(table_offset(4), 9), page_entry(MAPPED_VRAM, GENERIC_MEMORY));
    EXPECT_EQ(memory.entry(table_offset(4), 10), page_entry(MAPPED_VRAM + PAGE_BYTES, GENERIC_MEMORY));
    EXPECT_EQ(memory.words_holding(table_offset(4), 0), TABLE_WORDS - 2);

    // A second mapping in the same page table writes its two entries as one run
    memory_run next = {.address = MAPPED_VRAM + 2 * PAGE_BYTES, .bytes = 2 * PAGE_BYTES};
    ASSERT_EQ(tables.map(INDEXED_ADDRESS + 3 * PAGE_BYTES, &next, 1, GENERIC_MEMORY), OK);
    EXPECT_EQ(memory.allocations, 5u);
    EXPECT_EQ(memory.invalidates, 2u);

    ASSERT_EQ(memory.run_count, 1u);
    EXPECT_EQ(memory.run_offsets[0], table_offset(4) + 12 * 8);
    EXPECT_EQ(memory.entry(table_offset(4), 11), 0u);
    EXPECT_EQ(memory.entry(table_offset(4), 12), page_entry(MAPPED_VRAM + 2 * PAGE_BYTES, GENERIC_MEMORY));
    EXPECT_EQ(memory.entry(table_offset(4), 13), page_entry(MAPPED_VRAM + 3 * PAGE_BYTES, GENERIC_MEMORY));
}

TEST(nvidia_context_page_tables, a_mapping_and_an_unmap_across_2_mib_write_a_page_table_on_each_side) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);
    ASSERT_EQ(tables.create(), OK);

    memory_run run = {.address = MAPPED_VRAM, .bytes = 3 * PAGE_BYTES};
    ASSERT_EQ(tables.map(0x1FF000, &run, 1, {}), OK);
    EXPECT_EQ(memory.allocations, 6u);

    EXPECT_EQ(memory.entry(table_offset(3), 1), directory_entry(table_offset(4)));
    EXPECT_EQ(memory.entry(table_offset(3), 3), directory_entry(table_offset(5)));
    EXPECT_EQ(memory.entry(table_offset(4), 511), page_entry(MAPPED_VRAM));
    EXPECT_EQ(memory.entry(table_offset(5), 0), page_entry(MAPPED_VRAM + PAGE_BYTES));
    EXPECT_EQ(memory.entry(table_offset(5), 1), page_entry(MAPPED_VRAM + 2 * PAGE_BYTES));

    // Each page table's part of the unmap is one run, and the page past its end stays mapped
    ASSERT_EQ(tables.unmap(0x1FF000, 2 * PAGE_BYTES), OK);
    ASSERT_EQ(memory.run_count, 2u);
    EXPECT_EQ(memory.entry(table_offset(4), 511), 0u);
    EXPECT_EQ(memory.entry(table_offset(5), 0), 0u);
    EXPECT_EQ(memory.entry(table_offset(5), 1), page_entry(MAPPED_VRAM + 2 * PAGE_BYTES));
}

TEST(nvidia_context_page_tables, system_memory_runs_map_one_after_another_with_their_mapping) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);
    ASSERT_EQ(tables.create(), OK);

    // The last run lies past what an entry naming video memory reaches
    page_mapping read_only_system = {.aperture = page_aperture::system_memory, .read_only = true};
    memory_run runs[] = {
        {.address = 0x80000000, .bytes = PAGE_BYTES},
        {.address = 0x1234000, .bytes = 2 * PAGE_BYTES},
        {.address = ENTRY_REACH_BYTES, .bytes = PAGE_BYTES},
    };

    ASSERT_EQ(tables.map(0x10000, runs, 3, read_only_system), OK);

    EXPECT_EQ(memory.entry(table_offset(4), 16), page_entry(0x80000000, read_only_system));
    EXPECT_EQ(memory.entry(table_offset(4), 17), page_entry(0x1234000, read_only_system));
    EXPECT_EQ(memory.entry(table_offset(4), 18), page_entry(0x1235000, read_only_system));
    EXPECT_EQ(memory.entry(table_offset(4), 19), page_entry(ENTRY_REACH_BYTES, read_only_system));
    EXPECT_EQ(memory.words_holding(table_offset(4), 0), TABLE_WORDS - 4);
}

// Each refused mapping adds no table and writes nothing
TEST(nvidia_context_page_tables, a_mapping_not_of_whole_pages_past_the_addresses_or_of_a_refused_entry_is_refused) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);
    ASSERT_EQ(tables.create(), OK);

    memory_run page = {.address = MAPPED_VRAM, .bytes = PAGE_BYTES};
    memory_run two_pages = {.address = MAPPED_VRAM, .bytes = 2 * PAGE_BYTES};
    EXPECT_EQ(tables.map(0x800, &page, 1, {}), ERR_INVALID_ARG);
    EXPECT_EQ(tables.map(0, &page, 0, {}), ERR_INVALID_ARG);
    EXPECT_EQ(tables.map(ADDRESS_SPACE_BYTES - PAGE_BYTES, &two_pages, 1, {}), ERR_INVALID_ARG);
    EXPECT_EQ(tables.map(0 - PAGE_BYTES, &two_pages, 1, {}), ERR_INVALID_ARG);

    // A refused second run after a first run that passes the checks, the last one wrapping past 2^64 to page 0
    memory_run empty[] = {page, {.address = MAPPED_VRAM, .bytes = 0}};
    memory_run part_page[] = {page, {.address = MAPPED_VRAM, .bytes = PAGE_BYTES + 0x800}};
    EXPECT_EQ(tables.map(0, empty, 2, {}), ERR_INVALID_ARG);
    EXPECT_EQ(tables.map(0, part_page, 2, {}), ERR_INVALID_ARG);

    memory_run past_vram_reach[] = {page, {.address = ENTRY_REACH_BYTES - PAGE_BYTES, .bytes = 2 * PAGE_BYTES}};
    memory_run wrapping[] = {page, {.address = 0 - PAGE_BYTES, .bytes = 2 * PAGE_BYTES}};
    EXPECT_EQ(tables.map(0, past_vram_reach, 2, {}), ERR_INVALID_ARG);
    EXPECT_EQ(tables.map(0, wrapping, 2, {}), ERR_INVALID_ARG);

    page_mapping system_memory = {.aperture = page_aperture::system_memory};
    memory_run past_system_reach = {.address = (1ULL << 58) - PAGE_BYTES, .bytes = 2 * PAGE_BYTES};
    EXPECT_EQ(tables.map(0, &past_system_reach, 1, system_memory), ERR_INVALID_ARG);
    EXPECT_EQ(tables.map(0, &page, 1, {.kind = 7}), ERR_INVALID_ARG);

    EXPECT_EQ(memory.allocations, 1u);
    EXPECT_EQ(memory.updates, 1u);
    EXPECT_FALSE(failed.is_set());
    EXPECT_EQ(tables.map(ADDRESS_SPACE_BYTES - PAGE_BYTES, &page, 1, {}), OK);
}

TEST(nvidia_context_page_tables, a_run_past_the_context_addresses_is_refused_before_the_sizes_wrap) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);
    ASSERT_EQ(tables.create(), OK);

    // 64 runs of 2^58 bytes and one page, whose sizes would wrap past 2^64 to one page, refused at the first run
    memory_run wrapping[65];
    for (memory_run& run : wrapping) {
        run = {.address = 0, .bytes = 1ULL << 58};
    }

    wrapping[64].bytes = PAGE_BYTES;
    page_mapping system_memory = {.aperture = page_aperture::system_memory};
    EXPECT_EQ(tables.map(0, wrapping, 65, system_memory), ERR_INVALID_ARG);
    EXPECT_EQ(memory.allocations, 1u);
}

TEST(nvidia_context_page_tables, an_allocation_failure_releases_the_added_tables_and_writes_nothing) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);
    ASSERT_EQ(tables.create(), OK);

    // Directories 2 and 1 are added, then directory 0 cannot be
    memory.allocations_left = 2;
    memory_run run = {.address = MAPPED_VRAM, .bytes = PAGE_BYTES};
    EXPECT_EQ(tables.map(0, &run, 1, {}), ERR_NO_MEMORY);
    EXPECT_EQ(memory.updates, 1u);
    EXPECT_FALSE(failed.is_set());

    EXPECT_EQ(memory.releases, 2u);
    EXPECT_TRUE(memory.is_released[1] && memory.is_released[2]);
    EXPECT_EQ(memory.refused_calls, 0u);

    // A retry adds four tables of its own, named from the root's entry 0
    memory.allocations_left = FAKE_TABLES;
    ASSERT_EQ(tables.map(0, &run, 1, {}), OK);
    EXPECT_EQ(memory.allocations, 7u);
    EXPECT_EQ(memory.entry(table_offset(0), 0), directory_entry(table_offset(3)));
    EXPECT_EQ(memory.entry(table_offset(6), 0), page_entry(MAPPED_VRAM));
}

TEST(nvidia_context_page_tables, an_unmap_makes_its_entries_invalid_and_one_where_no_table_lies_writes_nothing) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);
    ASSERT_EQ(tables.create(), OK);

    memory_run run = {.address = MAPPED_VRAM, .bytes = 3 * PAGE_BYTES};
    ASSERT_EQ(tables.map(0x10000, &run, 1, {}), OK);

    ASSERT_EQ(tables.unmap(0x11000, PAGE_BYTES), OK);
    EXPECT_EQ(memory.invalidates, 2u);
    ASSERT_EQ(memory.run_count, 1u);

    EXPECT_EQ(memory.entry(table_offset(4), 16), page_entry(MAPPED_VRAM));
    EXPECT_EQ(memory.entry(table_offset(4), 17), 0u);
    EXPECT_EQ(memory.entry(table_offset(4), 18), page_entry(MAPPED_VRAM + 2 * PAGE_BYTES));

    EXPECT_EQ(tables.unmap(1ULL << 40, 1ULL << 30), OK);
    EXPECT_EQ(memory.updates, 3u);

    EXPECT_EQ(tables.unmap(0x10800, PAGE_BYTES), ERR_INVALID_ARG);
    EXPECT_EQ(tables.unmap(0x10000, 0), ERR_INVALID_ARG);
    EXPECT_EQ(tables.unmap(0x10000, PAGE_BYTES + 0x800), ERR_INVALID_ARG);
    EXPECT_EQ(tables.unmap(ADDRESS_SPACE_BYTES - PAGE_BYTES, 2 * PAGE_BYTES), ERR_INVALID_ARG);
    EXPECT_EQ(tables.unmap(0x10000, 0 - 0x10000), ERR_INVALID_ARG);

    EXPECT_EQ(memory.updates, 3u);
    EXPECT_EQ(memory.allocations, 5u);
}

// Fails one context's update with `update_result`, expecting the mark both contexts share set, every later call of
// either refused and no table released
static void expect_a_failed_update_to_keep_every_table(int32_t update_result) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;

    // Two contexts on the GPU's one mark
    context_page_tables tables(memory, failed);
    context_page_tables other(memory, failed);
    ASSERT_EQ(tables.create(), OK);
    ASSERT_EQ(other.create(), OK);

    memory.update_result = update_result;
    memory_run run = {.address = MAPPED_VRAM, .bytes = PAGE_BYTES};
    EXPECT_EQ(tables.map(0, &run, 1, {}), update_result);
    EXPECT_TRUE(failed.is_set());

    memory.update_result = OK;
    EXPECT_EQ(other.map(0x10000, &run, 1, {}), ERR_GPU_FAILED);
    EXPECT_EQ(other.unmap(0, PAGE_BYTES), ERR_GPU_FAILED);
    EXPECT_EQ(other.destroy(), ERR_GPU_FAILED);
    EXPECT_EQ(tables.destroy(), ERR_GPU_FAILED);
    EXPECT_EQ(tables.create(), ERR_GPU_FAILED);

    EXPECT_EQ(memory.updates, 3u);
    EXPECT_EQ(memory.releases, 0u);
    EXPECT_EQ(tables.directory_offset(), 0u);
}

TEST(nvidia_context_page_tables, a_timed_out_or_refused_update_marks_the_gpu_failed_and_keeps_every_table) {
    expect_a_failed_update_to_keep_every_table(ERR_TIMEOUT);
    expect_a_failed_update_to_keep_every_table(ERR_BAD_STATE);
}

TEST(nvidia_context_page_tables, destroy_invalidates_before_it_releases_every_table) {
    table_video_memory memory;
    ASSERT_NOT_NULL(memory.words);
    failed_mark failed;
    context_page_tables tables(memory, failed);
    ASSERT_EQ(tables.create(), OK);

    memory_run run = {.address = MAPPED_VRAM, .bytes = 2 * PAGE_BYTES};
    ASSERT_EQ(tables.map(0x1FF000, &run, 1, {}), OK);

    ASSERT_EQ(tables.destroy(), OK);
    EXPECT_EQ(memory.invalidates, 2u);
    EXPECT_EQ(memory.invalidated_directory, table_offset(0));
    EXPECT_EQ(memory.run_count, 0u);

    EXPECT_EQ(memory.releases_at_last_update, 0u);
    EXPECT_EQ(memory.releases, 6u);
    EXPECT_EQ(memory.refused_calls, 0u);
    EXPECT_EQ(tables.directory_offset(), 0u);
}
