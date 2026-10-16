// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/gsp_boot_memory.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/protected_memory.h"
#include "dynpriv/dynpriv.h"
#include "fs/fs.h"
#include "fw/fw.h"
#include "mm/heap.h"
#include "mm/page_quarantine.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_gsp_boot_memory);

using drivers::nvidia::boot_file_header;
using drivers::nvidia::bootloader_layout;
using drivers::nvidia::booter_layout;
using drivers::nvidia::booter_signature;
using drivers::nvidia::check_boot_copies;
using drivers::nvidia::chip_gsp_reservation;
using drivers::nvidia::ERR_BAD_FIRMWARE;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::FIRMWARE_LOG_COUNT;
using drivers::nvidia::firmware_files;
using drivers::nvidia::frts_region;
using drivers::nvidia::gsp_boot_memory;
using drivers::nvidia::gsp_image_layout;
using drivers::nvidia::lay_out_protected_memory;
using drivers::nvidia::message_queues;
using drivers::nvidia::OK;
using drivers::nvidia::protected_memory_layout;
using drivers::nvidia::radix3_table_pages;
using drivers::nvidia::write_booter_load_image;
using drivers::nvidia::write_radix3_table;

// The 4 KiB page every part of the boot memory is laid out in, radix3 tables included, and the addresses one radix3
// page holds, restated here so the tests hold the memory to NVIDIA's page and entry sizes rather than to the driver's
static constexpr uint64_t PAGE_BYTES   = 0x1000;
static constexpr uint64_t PAGE_ENTRIES = 512;

// The most data pages a table maps, the entries of a full level 2 under a level 0 of one page
static constexpr uint64_t MAX_DATA_PAGES = PAGE_ENTRIES * PAGE_ENTRIES * PAGE_ENTRIES;

// A page count that a cut to 32 bits makes 1, so only a count kept in 64 bits refuses it
static constexpr uint64_t PAST_32_BITS_PAGES = (1ULL << 32) + 1;

// The directories down to the suite's own, in creation order. The suite creates only the missing ones and removes
// exactly those, so an installed package's files are never touched.
static const char* const FIXTURE_DIRECTORIES[] = {
    "/usr",
    "/usr/lib",
    "/usr/lib/firmware",
    "/usr/lib/firmware/nvidia_boot_memory_test",
};

static constexpr size_t FIXTURE_DIRECTORY_COUNT = sizeof(FIXTURE_DIRECTORIES) / sizeof(FIXTURE_DIRECTORIES[0]);

static bool g_created_directories[FIXTURE_DIRECTORY_COUNT];

// The files the copies are read from, by their names for `fw::file` and by their paths for `fs::`
static constexpr const char BOOTLOADER_NAME[]  = "nvidia_boot_memory_test/bootloader.bin";
static constexpr const char BOOTLOADER_PATH[]  = "/usr/lib/firmware/nvidia_boot_memory_test/bootloader.bin";
static constexpr const char GSP_IMAGE_NAME[]   = "nvidia_boot_memory_test/gsp.bin";
static constexpr const char GSP_IMAGE_PATH[]   = "/usr/lib/firmware/nvidia_boot_memory_test/gsp.bin";
static constexpr const char BOOTER_LOAD_NAME[] = "nvidia_boot_memory_test/booter_load.bin";
static constexpr const char BOOTER_LOAD_PATH[] = "/usr/lib/firmware/nvidia_boot_memory_test/booter_load.bin";

static int32_t remove_fixtures() {
    fs::unlink(BOOTLOADER_PATH);
    fs::unlink(GSP_IMAGE_PATH);
    fs::unlink(BOOTER_LOAD_PATH);

    for (size_t i = FIXTURE_DIRECTORY_COUNT; i > 0; i--) {
        if (g_created_directories[i - 1]) {
            fs::rmdir(FIXTURE_DIRECTORIES[i - 1]);
            g_created_directories[i - 1] = false;
        }
    }

    return 0;
}

static int32_t create_fixtures() {
    for (size_t i = 0; i < FIXTURE_DIRECTORY_COUNT; i++) {
        int32_t rc = fs::mkdir(FIXTURE_DIRECTORIES[i], fs::DEFAULT_DIRECTORY_MODE);
        if (rc != fs::OK && rc != fs::ERR_EXIST) {
            remove_fixtures();
            return rc;
        }

        g_created_directories[i] = rc == fs::OK;
    }

    return 0;
}

BEFORE_ALL(nvidia_gsp_boot_memory, create_fixtures);
AFTER_ALL(nvidia_gsp_boot_memory, remove_fixtures);

TEST(nvidia_gsp_boot_memory, radix3_table_takes_a_page_per_512_entries_of_each_level) {
    // gsp_ga10x.bin of 535.183.01 has a .fwimage section of 0x2460000 bytes, 9312 pages under 19 pages of level 2
    uint64_t ga10x_firmware_pages = 0x2460000 / PAGE_BYTES;

    EXPECT_EQ(radix3_table_pages(0), static_cast<uint64_t>(0));
    EXPECT_EQ(radix3_table_pages(1), static_cast<uint64_t>(1 + 1 + 1));
    EXPECT_EQ(radix3_table_pages(PAGE_ENTRIES), static_cast<uint64_t>(1 + 1 + 1));
    EXPECT_EQ(radix3_table_pages(PAGE_ENTRIES + 1), static_cast<uint64_t>(1 + 1 + 2));
    EXPECT_EQ(radix3_table_pages(ga10x_firmware_pages), static_cast<uint64_t>(1 + 1 + 19));
    EXPECT_EQ(radix3_table_pages(PAGE_ENTRIES * PAGE_ENTRIES), 1 + 1 + PAGE_ENTRIES);
    EXPECT_EQ(radix3_table_pages(PAGE_ENTRIES * PAGE_ENTRIES + 1), 1 + 2 + (PAGE_ENTRIES + 1));
    EXPECT_EQ(radix3_table_pages(MAX_DATA_PAGES), 1 + PAGE_ENTRIES + PAGE_ENTRIES * PAGE_ENTRIES);
    EXPECT_EQ(radix3_table_pages(MAX_DATA_PAGES + 1), static_cast<uint64_t>(0));
    EXPECT_EQ(radix3_table_pages(PAST_32_BITS_PAGES), static_cast<uint64_t>(0));
}

// Where the card reaches the tables the tests write, above 32 bits
static constexpr uint64_t TABLE_ADDRESS = 0x7654321000;

// Three runs out of address order, two of them above 32 bits, 530 pages together, so level 2 fills one page and
// 18 entries of a second
static constexpr uint64_t SPLIT_DATA_PAGES  = 3 + 510 + 17;
static constexpr uint64_t SPLIT_TABLE_PAGES = 1 + 1 + 2;

TEST(nvidia_gsp_boot_memory, radix3_table_lists_each_level_then_every_page_of_the_runs_in_order) {
    dma::buffer runs[] = {
        {.virt = 0, .phys = 0x12345670000, .size = 3 * PAGE_BYTES, .cached = false},
        {.virt = 0, .phys = 0x9876540000, .size = 510 * PAGE_BYTES, .cached = false},
        {.virt = 0, .phys = 0x10000, .size = 17 * PAGE_BYTES, .cached = false},
    };

    uint64_t* table = static_cast<uint64_t*>(heap::uzalloc(SPLIT_TABLE_PAGES * PAGE_BYTES));
    ASSERT_NOT_NULL(table);

    write_radix3_table(table, TABLE_ADDRESS, runs, 3);

    uint64_t* level_1 = table + PAGE_ENTRIES;
    uint64_t* level_2 = level_1 + PAGE_ENTRIES;

    EXPECT_EQ(table[0], TABLE_ADDRESS + PAGE_BYTES);
    EXPECT_EQ(table[1], static_cast<uint64_t>(0));
    EXPECT_EQ(level_1[0], TABLE_ADDRESS + 2 * PAGE_BYTES);
    EXPECT_EQ(level_1[1], TABLE_ADDRESS + 3 * PAGE_BYTES);
    EXPECT_EQ(level_1[2], static_cast<uint64_t>(0));

    uint64_t wrong_entries = 0;
    uint64_t entry = 0;
    for (const dma::buffer& run : runs) {
        for (uint64_t page = 0; page < run.size / PAGE_BYTES; page++) {
            wrong_entries += level_2[entry] != run.phys + page * PAGE_BYTES;
            entry++;
        }
    }

    EXPECT_EQ(entry, SPLIT_DATA_PAGES);
    EXPECT_EQ(wrong_entries, static_cast<uint64_t>(0));
    EXPECT_EQ(level_2[SPLIT_DATA_PAGES], static_cast<uint64_t>(0));

    heap::ufree(table);
}

// One data page more than 512 pages of level 2 map, so level 2 takes 513 pages and level 1 two
static constexpr uint64_t SPANNING_DATA_PAGES  = PAGE_ENTRIES * PAGE_ENTRIES + 1;
static constexpr uint64_t SPANNING_TABLE_PAGES = 1 + 2 + (PAGE_ENTRIES + 1);

TEST(nvidia_gsp_boot_memory, radix3_level_1_continues_on_its_second_page_past_512_level_2_pages) {
    dma::buffer run = {.virt = 0, .phys = 0x400000000, .size = SPANNING_DATA_PAGES * PAGE_BYTES, .cached = false};

    uint64_t* table = static_cast<uint64_t*>(heap::uzalloc(SPANNING_TABLE_PAGES * PAGE_BYTES));
    ASSERT_NOT_NULL(table);

    write_radix3_table(table, TABLE_ADDRESS, &run, 1);

    uint64_t* level_1 = table + PAGE_ENTRIES;
    uint64_t* level_2 = level_1 + 2 * PAGE_ENTRIES;
    uint64_t level_2_address = TABLE_ADDRESS + 3 * PAGE_BYTES;

    EXPECT_EQ(table[0], TABLE_ADDRESS + PAGE_BYTES);
    EXPECT_EQ(table[1], TABLE_ADDRESS + 2 * PAGE_BYTES);
    EXPECT_EQ(table[2], static_cast<uint64_t>(0));
    EXPECT_EQ(level_1[PAGE_ENTRIES - 1], level_2_address + (PAGE_ENTRIES - 1) * PAGE_BYTES);
    EXPECT_EQ(level_1[PAGE_ENTRIES], level_2_address + PAGE_ENTRIES * PAGE_BYTES);
    EXPECT_EQ(level_1[PAGE_ENTRIES + 1], static_cast<uint64_t>(0));
    EXPECT_EQ(level_2[0], run.phys);
    EXPECT_EQ(level_2[SPANNING_DATA_PAGES - 1], run.phys + (SPANNING_DATA_PAGES - 1) * PAGE_BYTES);
    EXPECT_EQ(level_2[SPANNING_DATA_PAGES], static_cast<uint64_t>(0));

    heap::ufree(table);
}

// Each copied range lies at an offset and has a size above 0xFFFF, ending inside a page, so a cut to 16 bits or a
// copy of whole pages fails. The firmware takes two runs, the second of 18 pages, its last page partly filled.
static constexpr uint64_t BOOTLOADER_PROGRAM_OFFSET = 0x10050;
static constexpr uint32_t BOOTLOADER_PROGRAM_BYTES  = 0x10F21;
static constexpr uint64_t BOOTLOADER_FILE_BYTES     = 0x21000;
static constexpr uint64_t SIGNATURE_FILE_OFFSET     = 0x10123;
static constexpr uint64_t COPIED_SIGNATURE_BYTES    = 0x10ABC;
static constexpr uint64_t FIRMWARE_FILE_OFFSET      = 0x21340;
static constexpr uint64_t COPIED_FIRMWARE_BYTES     = 0x211234;
static constexpr uint64_t COPIED_FIRMWARE_PAGES     = 512 + 18;
static constexpr uint64_t GSP_IMAGE_FILE_BYTES      = 0x233000;

// Different salts make the two files differ at most offsets, so a copy from the wrong file fails a test
static constexpr uint32_t BOOTLOADER_SALT = 0x5A17C3E9;
static constexpr uint32_t GSP_IMAGE_SALT  = 0x0B7E4D21;

// The byte at `offset` in a fixture, mixed from the offset's low 32 bits and the file's salt, so a range read from
// another offset in the file, such as one 64 KiB or 2 MiB away, differs in most of its bytes
static uint8_t fixture_byte(uint64_t offset, uint32_t salt) {
    uint32_t mixed = (static_cast<uint32_t>(offset) ^ salt) * 0x9E3779B1;
    mixed ^= mixed >> 15;
    mixed *= 0x85EBCA77;
    mixed ^= mixed >> 13;

    return static_cast<uint8_t>(mixed);
}

// Writes `size` fixture bytes salted with `salt` at `path`, truncating any file there rather than replacing it, so
// a handle already open on it reads only the new bytes
static bool write_fixture(const char* path, uint64_t size, uint32_t salt) {
    uint8_t* bytes = static_cast<uint8_t*>(heap::ualloc(size));
    if (!bytes) {
        return false;
    }

    for (uint64_t i = 0; i < size; i++) {
        bytes[i] = fixture_byte(i, salt);
    }

    fs::file* f = fs::open(path, fs::O_CREAT | fs::O_TRUNC | fs::O_WRONLY);
    ssize_t written = f ? fs::write(f, bytes, size) : -1;
    if (f) {
        fs::close(f);
    }

    heap::ufree(bytes);

    return written == static_cast<ssize_t>(size);
}

// Writes the fixture at `path` and opens it at `name` into `file_out`
static bool install(const char* path, const char* name, uint64_t size, uint32_t salt, fw::file& file_out) {
    return write_fixture(path, size, salt) && file_out.open(name) == fw::OK;
}

static bool install_boot_files(firmware_files& files) {
    bool has_bootloader = install(
        BOOTLOADER_PATH,
        BOOTLOADER_NAME,
        BOOTLOADER_FILE_BYTES,
        BOOTLOADER_SALT,
        files.bootloader
    );
    bool has_gsp_image = install(GSP_IMAGE_PATH, GSP_IMAGE_NAME, GSP_IMAGE_FILE_BYTES, GSP_IMAGE_SALT, files.gsp_image);

    return has_bootloader && has_gsp_image;
}

static constexpr boot_file_header BOOTLOADER_HEADER = {
    .magic             = 0x10DE,
    .header_version    = 1,
    .rounded_size      = static_cast<uint32_t>(BOOTLOADER_FILE_BYTES),
    .descriptor_offset = 24,
    .program_offset    = static_cast<uint32_t>(BOOTLOADER_PROGRAM_OFFSET),
    .program_size      = BOOTLOADER_PROGRAM_BYTES,
};

static constexpr gsp_image_layout GSP_IMAGE = {
    .firmware_file_offset  = FIRMWARE_FILE_OFFSET,
    .firmware_size         = COPIED_FIRMWARE_BYTES,
    .signature_file_offset = SIGNATURE_FILE_OFFSET,
    .signature_size        = COPIED_SIGNATURE_BYTES,
};

// The tests read copies through the kernel's cached mapping of all physical memory, which other mappings must
// match, so the card is coherent
static dma::device_profile coherent_card() {
    return dma::device_profile_test_access::build(64, true);
}

// How many of the copy's `bytes` bytes differ from the fixture's at `file_offset`
static uint64_t count_differing_bytes(const uint8_t* copy, uint64_t file_offset, uint64_t bytes, uint32_t salt) {
    uint64_t differing = 0;
    for (uint64_t i = 0; i < bytes; i++) {
        differing += copy[i] != fixture_byte(file_offset + i, salt);
    }

    return differing;
}

// How many of the copy's `bytes` bytes differ from the fixture's at `file_offset`, plus how many bytes of the rest
// of its last page are not zero
static uint64_t count_wrong_bytes(const uint8_t* copy, uint64_t file_offset, uint64_t bytes, uint32_t salt) {
    uint64_t wrong = count_differing_bytes(copy, file_offset, bytes, salt);

    uint64_t padded_bytes = (bytes + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;
    for (uint64_t i = bytes; i < padded_bytes; i++) {
        wrong += copy[i] != 0;
    }

    return wrong;
}

// The table page at `address`, read through the kernel's mapping of all physical memory
static const uint64_t* table_page(uint64_t address) {
    return static_cast<const uint64_t*>(paging::phys_to_virt(address));
}

TEST(nvidia_gsp_boot_memory, boot_file_parts_are_copied_whole_into_pages_ending_in_zeros) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);

    const uint8_t* bootloader = static_cast<const uint8_t*>(paging::phys_to_virt(memory.bootloader_address()));
    const uint8_t* signature = static_cast<const uint8_t*>(paging::phys_to_virt(memory.signature_address()));
    uint64_t wrong_bootloader_bytes = count_wrong_bytes(
        bootloader,
        BOOTLOADER_PROGRAM_OFFSET,
        BOOTLOADER_PROGRAM_BYTES,
        BOOTLOADER_SALT
    );

    uint64_t wrong_signature_bytes = count_wrong_bytes(
        signature,
        SIGNATURE_FILE_OFFSET,
        COPIED_SIGNATURE_BYTES,
        GSP_IMAGE_SALT
    );

    EXPECT_EQ(wrong_bootloader_bytes, static_cast<uint64_t>(0));
    EXPECT_EQ(wrong_signature_bytes, static_cast<uint64_t>(0));

    // The firmware is reached as the card reaches it, through the table's levels
    const uint64_t* level_0 = table_page(memory.radix3_address());
    const uint64_t* level_1 = table_page(level_0[0]);
    const uint64_t* level_2[] = {table_page(level_1[0]), table_page(level_1[1])};

    EXPECT_EQ(level_0[1], static_cast<uint64_t>(0));
    EXPECT_EQ(level_1[2], static_cast<uint64_t>(0));
    EXPECT_EQ(level_2[1][COPIED_FIRMWARE_PAGES - PAGE_ENTRIES], static_cast<uint64_t>(0));

    uint64_t wrong_firmware_bytes = 0;
    for (uint64_t page = 0; page < COPIED_FIRMWARE_PAGES; page++) {
        uint64_t copied_bytes = page * PAGE_BYTES;
        uint64_t left_bytes = COPIED_FIRMWARE_BYTES - copied_bytes;
        uint64_t page_bytes = left_bytes < PAGE_BYTES ? left_bytes : PAGE_BYTES;
        uint64_t file_offset = FIRMWARE_FILE_OFFSET + copied_bytes;
        uint64_t page_address = level_2[page / PAGE_ENTRIES][page % PAGE_ENTRIES];
        const uint8_t* copy = static_cast<const uint8_t*>(paging::phys_to_virt(page_address));

        wrong_firmware_bytes += count_wrong_bytes(copy, file_offset, page_bytes, GSP_IMAGE_SALT);
    }

    EXPECT_EQ(wrong_firmware_bytes, static_cast<uint64_t>(0));

    memory.release();

    EXPECT_EQ(memory.radix3_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.signature_address(), static_cast<uint64_t>(0));
}

static uint64_t drained_free_pages() {
    page_quarantine::drain();
    return pmm::free_page_count();
}

TEST(nvidia_gsp_boot_memory, bootloader_program_of_no_byte_is_refused_leaving_free_memory_unchanged) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    boot_file_header empty_program = BOOTLOADER_HEADER;
    empty_program.program_size = 0;

    uint64_t free_pages_before = drained_free_pages();
    gsp_boot_memory memory;

    EXPECT_EQ(memory.copy_boot_files(coherent_card(), files, empty_program, GSP_IMAGE), ERR_BAD_FIRMWARE);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
}

TEST(nvidia_gsp_boot_memory, firmware_of_more_pages_than_a_radix3_table_maps_is_refused_leaving_free_memory_unchanged) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    gsp_image_layout unmappable = GSP_IMAGE;
    unmappable.firmware_size = MAX_DATA_PAGES * PAGE_BYTES + 1;

    uint64_t free_pages_before = drained_free_pages();
    gsp_boot_memory memory;

    int32_t rc = memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, unmappable);

    EXPECT_EQ(rc, ERR_BAD_FIRMWARE);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.radix3_address(), static_cast<uint64_t>(0));

    unmappable.firmware_size = PAST_32_BITS_PAGES * PAGE_BYTES;
    EXPECT_EQ(check_boot_copies(BOOTLOADER_HEADER, unmappable), ERR_BAD_FIRMWARE);

    // These differ by one byte, so a page count rounding a partial last page down or a whole one up fails one of them
    gsp_image_layout largest_mappable = GSP_IMAGE;
    largest_mappable.firmware_size = MAX_DATA_PAGES * PAGE_BYTES;
    gsp_image_layout one_byte_more = largest_mappable;
    one_byte_more.firmware_size++;

    EXPECT_EQ(check_boot_copies(BOOTLOADER_HEADER, largest_mappable), OK);
    EXPECT_EQ(check_boot_copies(BOOTLOADER_HEADER, one_byte_more), ERR_BAD_FIRMWARE);
}

TEST(nvidia_gsp_boot_memory, copying_the_bootloader_and_gsp_image_again_is_refused_keeping_the_first_copies) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    uint64_t radix3_address = memory.radix3_address();
    uint64_t bootloader_address = memory.bootloader_address();
    uint64_t signature_address = memory.signature_address();

    EXPECT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), ERR_BAD_STATE);
    EXPECT_EQ(memory.radix3_address(), radix3_address);
    EXPECT_EQ(memory.bootloader_address(), bootloader_address);
    EXPECT_EQ(memory.signature_address(), signature_address);

    memory.release();
}

// A limit below 32 bits is one the DMA layer refuses, so the first allocation fails
static constexpr uint8_t REFUSED_ADDRESS_BITS = 31;

TEST(nvidia_gsp_boot_memory, failed_allocation_returns_no_memory_and_takes_no_page) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    dma::device_profile refused_card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);
    uint64_t free_pages_before = drained_free_pages();
    gsp_boot_memory memory;

    EXPECT_EQ(memory.copy_boot_files(refused_card, files, BOOTLOADER_HEADER, GSP_IMAGE), ERR_NO_MEMORY);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
}

// Pages the kernel may keep for page tables that mapped the memory, fewer than the smallest part, the table's 4
static constexpr uint64_t KERNEL_PAGE_TABLE_PAGES = 3;

// Rewrites the file at `path` to its first `cut_bytes` bytes, shrinking the file `files` holds open, so a read past
// the cut fails, then expects the copy to fail with `ERR_BAD_FIRMWARE`, giving back every page it took
static void expect_cut_file_to_free_every_part(const char* path, uint32_t salt, uint64_t cut_bytes) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    // A first copy maps memory the way the failing one does, so its page tables already exist
    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    memory.release();

    ASSERT_TRUE(write_fixture(path, cut_bytes, salt));

    uint64_t free_pages_before = drained_free_pages();

    EXPECT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), ERR_BAD_FIRMWARE);
    EXPECT_GE(drained_free_pages() + KERNEL_PAGE_TABLE_PAGES, free_pages_before);
    EXPECT_EQ(memory.radix3_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.signature_address(), static_cast<uint64_t>(0));
}

TEST(nvidia_gsp_boot_memory, failed_read_of_the_bootloader_program_frees_every_part_allocated) {
    uint64_t cut_bytes = BOOTLOADER_PROGRAM_OFFSET + 0x100;
    expect_cut_file_to_free_every_part(BOOTLOADER_PATH, BOOTLOADER_SALT, cut_bytes);
}

TEST(nvidia_gsp_boot_memory, failed_read_of_the_signature_frees_every_part_allocated) {
    uint64_t cut_bytes = SIGNATURE_FILE_OFFSET + 0x100;
    expect_cut_file_to_free_every_part(GSP_IMAGE_PATH, GSP_IMAGE_SALT, cut_bytes);
}

// A cut inside the first run fails its read before the second run is allocated, so `release` meets an empty entry
// in the list of runs
TEST(nvidia_gsp_boot_memory, failed_read_of_the_first_firmware_run_frees_every_part_allocated) {
    uint64_t cut_bytes = FIRMWARE_FILE_OFFSET + 0x100;
    expect_cut_file_to_free_every_part(GSP_IMAGE_PATH, GSP_IMAGE_SALT, cut_bytes);
}

// The driver copies the firmware in runs of 2 MiB, so a GSP image cut just past the first run fails the second
// run's read after every part, the second run included, was allocated
static constexpr uint64_t FIRST_RUN_BYTES = 0x200000;

TEST(nvidia_gsp_boot_memory, failed_read_of_the_second_firmware_run_frees_every_part_allocated) {
    uint64_t cut_bytes = FIRMWARE_FILE_OFFSET + FIRST_RUN_BYTES + 0x100;
    expect_cut_file_to_free_every_part(GSP_IMAGE_PATH, GSP_IMAGE_SALT, cut_bytes);
}

// Firmware of the most pages a table maps needs a table of 1 + 512 + 512 x 512 pages, more than a single DMA
// allocation holds, so the table's allocation fails after the bootloader's program and the signature were copied
TEST(nvidia_gsp_boot_memory, failed_allocation_of_the_radix3_table_frees_every_part_allocated) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    gsp_image_layout largest_mappable = GSP_IMAGE;
    largest_mappable.firmware_size = MAX_DATA_PAGES * PAGE_BYTES;

    // A first copy maps memory the way the failing one does, so its page tables already exist
    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    memory.release();

    uint64_t free_pages_before = drained_free_pages();

    EXPECT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, largest_mappable), ERR_NO_MEMORY);
    EXPECT_GE(drained_free_pages() + KERNEL_PAGE_TABLE_PAGES, free_pages_before);
    EXPECT_EQ(memory.radix3_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.signature_address(), static_cast<uint64_t>(0));
}

// Every offset and size the copy reads lies above 0xFFFF, so a field cut to 16 bits fails, a data offset of 0x12300
// fails a slot not counted from the data's start, and the signatures lie before the program, as in booter load's file
static constexpr uint32_t BOOTER_SIGNATURES_OFFSET = 0x10040;
static constexpr uint32_t BOOTER_SIGNATURE_BYTES   = 0x10180;
static constexpr uint32_t BOOTER_SIGNATURE_COUNT   = 4;
static constexpr uint64_t BOOTER_PROGRAM_OFFSET    = 0x50700;
static constexpr uint32_t BOOTER_PROGRAM_BYTES     = 0x40E51;
static constexpr uint32_t BOOTER_DATA_OFFSET       = 0x12300;
static constexpr uint32_t BOOTER_SIGNATURE_SLOT    = 0x10088;
static constexpr uint64_t BOOTER_LOAD_FILE_BYTES   = 0xD2000;
static constexpr uint32_t BOOTER_LOAD_SALT         = 0x3C6EF372;

static constexpr boot_file_header BOOTER_LOAD_HEADER = {
    .magic             = 0x10DE,
    .header_version    = 1,
    .rounded_size      = static_cast<uint32_t>(BOOTER_LOAD_FILE_BYTES),
    .descriptor_offset = 24,
    .program_offset    = static_cast<uint32_t>(BOOTER_PROGRAM_OFFSET),
    .program_size      = BOOTER_PROGRAM_BYTES,
};

static constexpr booter_layout BOOTER_LAYOUT = {
    .code_offset       = 0x100,
    .code_size         = 0x12200,
    .data_offset       = BOOTER_DATA_OFFSET,
    .data_size         = 0x20B00,
    .signature_slot    = BOOTER_SIGNATURE_SLOT,
    .signatures_offset = BOOTER_SIGNATURES_OFFSET,
    .signature_size    = BOOTER_SIGNATURE_BYTES,
    .signature_count   = BOOTER_SIGNATURE_COUNT,
    .ucode_id          = 3,
    .engine_mask       = 0x1,
};

// Index 1 of 4 is neither the fuse version, 0, nor the last index, which NVIDIA's rule gives for that version, so a
// patch taking any signature but the chosen one fails
static constexpr booter_signature CHOSEN_SIGNATURE = {.fuse_version = 0, .index = 1};

// Where the slot starts in the program and where the chosen signature starts in the file
static constexpr uint64_t BOOTER_SLOT_OFFSET      = BOOTER_DATA_OFFSET + BOOTER_SIGNATURE_SLOT;
static constexpr uint64_t CHOSEN_SIGNATURE_OFFSET = BOOTER_SIGNATURES_OFFSET + 1 * BOOTER_SIGNATURE_BYTES;

// The same layout with the signatures after the program, so a cut can leave the whole program and fail only the
// chosen signature's read
static constexpr uint32_t LATE_SIGNATURES_OFFSET       = 0x91600;
static constexpr uint64_t LATE_CHOSEN_SIGNATURE_OFFSET = LATE_SIGNATURES_OFFSET + 1 * BOOTER_SIGNATURE_BYTES;

static constexpr booter_layout late_signatures_layout() {
    booter_layout layout = BOOTER_LAYOUT;
    layout.signatures_offset = LATE_SIGNATURES_OFFSET;
    return layout;
}

// Bytes past the program's end that a write past it would change
static constexpr uint64_t PROGRAM_GUARD_BYTES = 0x100;
static constexpr uint8_t  PROGRAM_GUARD_BYTE  = 0xA5;

static bool install_booter_load(fw::file& file_out) {
    return install(BOOTER_LOAD_PATH, BOOTER_LOAD_NAME, BOOTER_LOAD_FILE_BYTES, BOOTER_LOAD_SALT, file_out);
}

static int32_t copy_booter_load(gsp_boot_memory& memory, const dma::device_profile& device, const fw::file& file) {
    return memory.copy_booter_load(device, file, BOOTER_LOAD_HEADER, BOOTER_LAYOUT, CHOSEN_SIGNATURE);
}

TEST(nvidia_gsp_boot_memory, booter_load_image_holds_its_program_with_the_chosen_signature_over_its_slot) {
    fw::file booter_load;
    ASSERT_TRUE(install_booter_load(booter_load));

    uint8_t* image = static_cast<uint8_t*>(heap::ualloc(BOOTER_PROGRAM_BYTES + PROGRAM_GUARD_BYTES));
    ASSERT_NOT_NULL(image);
    string::memset(image, PROGRAM_GUARD_BYTE, BOOTER_PROGRAM_BYTES + PROGRAM_GUARD_BYTES);

    int32_t rc = write_booter_load_image(booter_load, BOOTER_LOAD_HEADER, BOOTER_LAYOUT, CHOSEN_SIGNATURE, image);

    uint64_t slot_end = BOOTER_SLOT_OFFSET + BOOTER_SIGNATURE_BYTES;
    uint64_t wrong_before_slot = count_differing_bytes(
        image,
        BOOTER_PROGRAM_OFFSET,
        BOOTER_SLOT_OFFSET,
        BOOTER_LOAD_SALT
    );
    uint64_t wrong_in_slot = count_differing_bytes(
        image + BOOTER_SLOT_OFFSET,
        CHOSEN_SIGNATURE_OFFSET,
        BOOTER_SIGNATURE_BYTES,
        BOOTER_LOAD_SALT
    );
    uint64_t wrong_after_slot = count_differing_bytes(
        image + slot_end,
        BOOTER_PROGRAM_OFFSET + slot_end,
        BOOTER_PROGRAM_BYTES - slot_end,
        BOOTER_LOAD_SALT
    );

    uint64_t changed_guard_bytes = 0;
    for (uint64_t i = BOOTER_PROGRAM_BYTES; i < BOOTER_PROGRAM_BYTES + PROGRAM_GUARD_BYTES; i++) {
        changed_guard_bytes += image[i] != PROGRAM_GUARD_BYTE;
    }

    EXPECT_EQ(rc, OK);
    EXPECT_EQ(wrong_before_slot, static_cast<uint64_t>(0));
    EXPECT_EQ(wrong_in_slot, static_cast<uint64_t>(0));
    EXPECT_EQ(wrong_after_slot, static_cast<uint64_t>(0));
    EXPECT_EQ(changed_guard_bytes, static_cast<uint64_t>(0));

    heap::ufree(image);
}

// The DMA layer takes blocks of a power of two pages (vmm.h), so booter load's 65 pages take 128 and the queues' 129
// take 256, while a request a page short of either takes half as many
static constexpr uint64_t BOOTER_PROGRAM_BLOCK_PAGES = 128;
static constexpr uint64_t QUEUE_BLOCK_PAGES          = 256;

TEST(nvidia_gsp_boot_memory, booter_load_copy_and_message_queues_hold_their_pages_until_release) {
    fw::file booter_load;
    ASSERT_TRUE(install_booter_load(booter_load));

    uint64_t free_pages_before = drained_free_pages();
    gsp_boot_memory memory;

    ASSERT_EQ(copy_booter_load(memory, coherent_card(), booter_load), OK);
    uint64_t free_pages_with_booter_load = drained_free_pages();
    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);
    uint64_t free_pages_with_both = drained_free_pages();

    EXPECT_NE(memory.booter_load_address(), static_cast<uint64_t>(0));
    EXPECT_NE(memory.message_queues_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.booter_load_address() % PAGE_BYTES, static_cast<uint64_t>(0));
    EXPECT_EQ(memory.message_queues_address() % PAGE_BYTES, static_cast<uint64_t>(0));
    EXPECT_GE(free_pages_before - free_pages_with_booter_load, BOOTER_PROGRAM_BLOCK_PAGES);
    EXPECT_GE(free_pages_with_booter_load - free_pages_with_both, QUEUE_BLOCK_PAGES);

    memory.release();

    EXPECT_EQ(memory.booter_load_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.message_queues_address(), static_cast<uint64_t>(0));
    EXPECT_GE(drained_free_pages() + KERNEL_PAGE_TABLE_PAGES, free_pages_before);
}

// Cuts booter load to its first `cut_bytes` bytes, so a read past the cut fails, then expects copying it with `layout`
// into an object holding the message queues' memory to fail, giving back only its own copy's pages
static void expect_cut_booter_load_to_free_only_its_copy(const booter_layout& layout, uint64_t cut_bytes) {
    fw::file booter_load;
    ASSERT_TRUE(install_booter_load(booter_load));

    gsp_boot_memory memory;
    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);
    uint64_t message_queues_address = memory.message_queues_address();

    // A first copy maps memory the way the failing one does, so its page tables already exist
    gsp_boot_memory first_copy;
    int32_t first_rc = first_copy.copy_booter_load(
        coherent_card(),
        booter_load,
        BOOTER_LOAD_HEADER,
        layout,
        CHOSEN_SIGNATURE
    );
    ASSERT_EQ(first_rc, OK);
    first_copy.release();

    ASSERT_TRUE(write_fixture(BOOTER_LOAD_PATH, cut_bytes, BOOTER_LOAD_SALT));
    uint64_t free_pages_before = drained_free_pages();

    int32_t rc = memory.copy_booter_load(coherent_card(), booter_load, BOOTER_LOAD_HEADER, layout, CHOSEN_SIGNATURE);

    EXPECT_EQ(rc, ERR_BAD_FIRMWARE);
    EXPECT_GE(drained_free_pages() + KERNEL_PAGE_TABLE_PAGES, free_pages_before);
    EXPECT_EQ(memory.booter_load_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.message_queues_address(), message_queues_address);

    memory.release();
}

// The cut leaves the signatures before the program whole, so the copy fails only if it stops at the program's read
TEST(nvidia_gsp_boot_memory, failed_read_of_booter_load_program_frees_only_its_copy) {
    expect_cut_booter_load_to_free_only_its_copy(BOOTER_LAYOUT, BOOTER_PROGRAM_OFFSET + 0x100);
}

TEST(nvidia_gsp_boot_memory, failed_read_of_booter_load_signature_frees_only_its_copy) {
    expect_cut_booter_load_to_free_only_its_copy(late_signatures_layout(), LATE_CHOSEN_SIGNATURE_OFFSET + 0x100);
}

// The refused profile fails the first allocation of `copy_boot_files`, before the bootloader or the GSP image is
// read, so only booter load is installed
TEST(nvidia_gsp_boot_memory, failed_copy_of_the_bootloader_and_gsp_image_keeps_booter_load_and_the_message_queues) {
    firmware_files files;
    ASSERT_TRUE(install_booter_load(files.booter_load));

    gsp_boot_memory memory;
    ASSERT_EQ(copy_booter_load(memory, coherent_card(), files.booter_load), OK);
    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);
    uint64_t booter_load_address = memory.booter_load_address();
    uint64_t message_queues_address = memory.message_queues_address();

    dma::device_profile refused_card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);

    EXPECT_EQ(memory.copy_boot_files(refused_card, files, BOOTLOADER_HEADER, GSP_IMAGE), ERR_NO_MEMORY);
    EXPECT_EQ(memory.booter_load_address(), booter_load_address);
    EXPECT_EQ(memory.message_queues_address(), message_queues_address);

    memory.release();
}

TEST(nvidia_gsp_boot_memory, failed_booter_load_or_queue_allocation_keeps_the_bootloader_and_gsp_image_copies) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));
    ASSERT_TRUE(install_booter_load(files.booter_load));

    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    uint64_t bootloader_address = memory.bootloader_address();

    dma::device_profile refused_card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);
    uint64_t free_pages_before = drained_free_pages();

    EXPECT_EQ(copy_booter_load(memory, refused_card, files.booter_load), ERR_NO_MEMORY);
    EXPECT_EQ(memory.allocate_message_queues(refused_card), ERR_NO_MEMORY);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.booter_load_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.message_queues_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.bootloader_address(), bootloader_address);

    memory.release();
}

TEST(nvidia_gsp_boot_memory, filling_booter_load_or_the_message_queues_again_is_refused_keeping_the_first) {
    fw::file booter_load;
    ASSERT_TRUE(install_booter_load(booter_load));

    gsp_boot_memory memory;
    ASSERT_EQ(copy_booter_load(memory, coherent_card(), booter_load), OK);
    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);
    uint64_t booter_load_address = memory.booter_load_address();
    uint64_t message_queues_address = memory.message_queues_address();

    EXPECT_EQ(copy_booter_load(memory, coherent_card(), booter_load), ERR_BAD_STATE);
    EXPECT_EQ(memory.allocate_message_queues(coherent_card()), ERR_BAD_STATE);
    EXPECT_EQ(memory.booter_load_address(), booter_load_address);
    EXPECT_EQ(memory.message_queues_address(), message_queues_address);

    memory.release();
}

// A card with 10 GiB of video memory whose VGA workspace takes its last 128 KiB, so the workspace's size differs from
// the FRTS region's 1 MiB
static constexpr frts_region METADATA_REGION = {
    .video_memory_size    = 0x280000000,
    .vga_workspace_offset = 0x27FFE0000,
    .offset               = 0x27FEE0000,
};

static constexpr uint64_t METADATA_VGA_WORKSPACE_BYTES = 0x20000;
static constexpr uint64_t METADATA_FRTS_BYTES          = 0x100000;

// Invented sizes for the heaps and the prescrubbed top, with a 2.75 MiB unprotected heap whose size differs from the
// FRTS region's 1 MiB
static constexpr chip_gsp_reservation METADATA_RESERVATION = {
    .heap_os_bytes          = 0x1000000,
    .heap_base_bytes        = 0x400000,
    .heap_min_bytes         = 0x2000000,
    .unprotected_heap_bytes = 0x2C0000,
    .prescrubbed_top_bytes  = 0x20000000,
};

// Offsets past 16 bits inside the bootloader's 0x10F21-byte program
static constexpr bootloader_layout METADATA_BOOTLOADER_PARTS = {
    .manifest_offset     = 0x10C00,
    .monitor_data_offset = 0x10400,
    .monitor_code_offset = 0x10200,
    .app_version         = 0,
};

// The signature's 0x10ABC bytes in whole 256-byte blocks
static constexpr uint64_t METADATA_SIGNATURE_BYTES = 0x10B00;

// NVIDIA's metadata layout, restated here so the tests hold the page to NVIDIA's offsets rather than to the driver's
// structure. Every field from the boot count to the end is 0 on NVIDIA's first attempt to boot the GSP by default
static constexpr uint64_t METADATA_MAGIC                = 0xDC3AAE21371A60B3;
static constexpr uint64_t METADATA_REVISION             = 1;
static constexpr uint64_t METADATA_BYTES                = 256;
static constexpr uint64_t MAGIC_AT                      = 0;
static constexpr uint64_t REVISION_AT                   = 8;
static constexpr uint64_t RADIX3_ADDRESS_AT             = 16;
static constexpr uint64_t FIRMWARE_BYTES_AT             = 24;
static constexpr uint64_t BOOTLOADER_ADDRESS_AT         = 32;
static constexpr uint64_t BOOTLOADER_BYTES_AT           = 40;
static constexpr uint64_t BOOTLOADER_CODE_OFFSET_AT     = 48;
static constexpr uint64_t BOOTLOADER_DATA_OFFSET_AT     = 56;
static constexpr uint64_t BOOTLOADER_MANIFEST_OFFSET_AT = 64;
static constexpr uint64_t SIGNATURE_ADDRESS_AT          = 72;
static constexpr uint64_t SIGNATURE_BYTES_AT            = 80;
static constexpr uint64_t RESERVED_START_AT             = 88;
static constexpr uint64_t UNPROTECTED_HEAP_OFFSET_AT    = 96;
static constexpr uint64_t UNPROTECTED_HEAP_BYTES_AT     = 104;
static constexpr uint64_t PROTECTED_START_AT            = 112;
static constexpr uint64_t HEAP_OFFSET_AT                = 120;
static constexpr uint64_t HEAP_BYTES_AT                 = 128;
static constexpr uint64_t FIRMWARE_OFFSET_AT            = 136;
static constexpr uint64_t BOOTLOADER_OFFSET_AT          = 144;
static constexpr uint64_t FRTS_OFFSET_AT                = 152;
static constexpr uint64_t FRTS_BYTES_AT                 = 160;
static constexpr uint64_t PROTECTED_END_AT              = 168;
static constexpr uint64_t VIDEO_MEMORY_SIZE_AT          = 176;
static constexpr uint64_t VGA_WORKSPACE_OFFSET_AT       = 184;
static constexpr uint64_t VGA_WORKSPACE_BYTES_AT        = 192;
static constexpr uint64_t BOOT_COUNT_AT                 = 200;

static int32_t lay_out_for_metadata(protected_memory_layout& layout_out) {
    return lay_out_protected_memory(
        METADATA_REGION,
        METADATA_RESERVATION,
        BOOTLOADER_PROGRAM_BYTES,
        COPIED_FIRMWARE_BYTES,
        layout_out
    );
}

static int32_t write_metadata(
    gsp_boot_memory& memory,
    const dma::device_profile& device,
    const protected_memory_layout& layout
) {
    return memory.write_protected_metadata(device, METADATA_REGION, layout, METADATA_BOOTLOADER_PARTS);
}

// The 64-bit field at `offset` in the metadata's page, read through the kernel's mapping of all physical memory
static uint64_t metadata_field(const gsp_boot_memory& memory, uint64_t offset) {
    const uint8_t* page = static_cast<const uint8_t*>(paging::phys_to_virt(memory.protected_metadata_address()));
    uint64_t value = 0;
    string::memcpy(&value, page + offset, sizeof(value));

    return value;
}

TEST(nvidia_gsp_boot_memory, protected_metadata_holds_each_field_at_nvidias_offset) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    protected_memory_layout layout = {};
    ASSERT_EQ(lay_out_for_metadata(layout), OK);

    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    ASSERT_EQ(write_metadata(memory, coherent_card(), layout), OK);

    EXPECT_EQ(memory.protected_metadata_address() % PAGE_BYTES, static_cast<uint64_t>(0));
    EXPECT_EQ(metadata_field(memory, MAGIC_AT), METADATA_MAGIC);
    EXPECT_EQ(metadata_field(memory, REVISION_AT), METADATA_REVISION);

    EXPECT_EQ(metadata_field(memory, RADIX3_ADDRESS_AT), memory.radix3_address());
    EXPECT_EQ(metadata_field(memory, FIRMWARE_BYTES_AT), COPIED_FIRMWARE_BYTES);
    EXPECT_EQ(metadata_field(memory, BOOTLOADER_ADDRESS_AT), memory.bootloader_address());
    EXPECT_EQ(metadata_field(memory, BOOTLOADER_BYTES_AT), static_cast<uint64_t>(BOOTLOADER_PROGRAM_BYTES));
    EXPECT_EQ(metadata_field(memory, SIGNATURE_ADDRESS_AT), memory.signature_address());
    EXPECT_EQ(metadata_field(memory, SIGNATURE_BYTES_AT), METADATA_SIGNATURE_BYTES);

    uint64_t code_offset = METADATA_BOOTLOADER_PARTS.monitor_code_offset;
    uint64_t data_offset = METADATA_BOOTLOADER_PARTS.monitor_data_offset;
    uint64_t manifest_offset = METADATA_BOOTLOADER_PARTS.manifest_offset;
    EXPECT_EQ(metadata_field(memory, BOOTLOADER_CODE_OFFSET_AT), code_offset);
    EXPECT_EQ(metadata_field(memory, BOOTLOADER_DATA_OFFSET_AT), data_offset);
    EXPECT_EQ(metadata_field(memory, BOOTLOADER_MANIFEST_OFFSET_AT), manifest_offset);

    uint64_t unprotected_heap_bytes = METADATA_RESERVATION.unprotected_heap_bytes;
    EXPECT_EQ(metadata_field(memory, RESERVED_START_AT), layout.unprotected_heap_offset);
    EXPECT_EQ(metadata_field(memory, UNPROTECTED_HEAP_OFFSET_AT), layout.unprotected_heap_offset);
    EXPECT_EQ(metadata_field(memory, UNPROTECTED_HEAP_BYTES_AT), unprotected_heap_bytes);
    EXPECT_EQ(metadata_field(memory, PROTECTED_START_AT), layout.protected_start);
    EXPECT_EQ(metadata_field(memory, HEAP_OFFSET_AT), layout.heap_offset);
    EXPECT_EQ(metadata_field(memory, HEAP_BYTES_AT), layout.heap_bytes);

    EXPECT_EQ(metadata_field(memory, FIRMWARE_OFFSET_AT), layout.firmware_offset);
    EXPECT_EQ(metadata_field(memory, BOOTLOADER_OFFSET_AT), layout.bootloader_offset);
    EXPECT_EQ(metadata_field(memory, FRTS_OFFSET_AT), METADATA_REGION.offset);
    EXPECT_EQ(metadata_field(memory, FRTS_BYTES_AT), METADATA_FRTS_BYTES);
    EXPECT_EQ(metadata_field(memory, PROTECTED_END_AT), METADATA_REGION.offset + METADATA_FRTS_BYTES);

    EXPECT_EQ(metadata_field(memory, VIDEO_MEMORY_SIZE_AT), METADATA_REGION.video_memory_size);
    EXPECT_EQ(metadata_field(memory, VGA_WORKSPACE_OFFSET_AT), METADATA_REGION.vga_workspace_offset);
    EXPECT_EQ(metadata_field(memory, VGA_WORKSPACE_BYTES_AT), METADATA_VGA_WORKSPACE_BYTES);

    uint64_t nonzero_fields = 0;
    for (uint64_t offset = BOOT_COUNT_AT; offset < METADATA_BYTES; offset += sizeof(uint64_t)) {
        nonzero_fields += metadata_field(memory, offset) != 0;
    }

    EXPECT_EQ(nonzero_fields, static_cast<uint64_t>(0));

    memory.release();

    EXPECT_EQ(memory.protected_metadata_address(), static_cast<uint64_t>(0));
}

// A signature size on a 256-byte boundary is kept, and one a byte past it takes the whole next block, which rounding
// to the nearest block, to 128 bytes or to 512 bytes would not give
static constexpr uint64_t WHOLE_BLOCKS_SIGNATURE_BYTES = 0x10A00;
static constexpr uint64_t PAST_A_BLOCK_SIGNATURE_BYTES = 0x10A01;
static constexpr uint64_t NEXT_BLOCK_SIGNATURE_BYTES   = 0x10B00;

TEST(nvidia_gsp_boot_memory, protected_metadata_rounds_the_signature_size_up_to_whole_blocks) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    protected_memory_layout layout = {};
    ASSERT_EQ(lay_out_for_metadata(layout), OK);

    gsp_image_layout image = GSP_IMAGE;
    image.signature_size = WHOLE_BLOCKS_SIGNATURE_BYTES;

    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, image), OK);
    ASSERT_EQ(write_metadata(memory, coherent_card(), layout), OK);

    EXPECT_EQ(metadata_field(memory, SIGNATURE_BYTES_AT), WHOLE_BLOCKS_SIGNATURE_BYTES);

    memory.release();
    image.signature_size = PAST_A_BLOCK_SIGNATURE_BYTES;

    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, image), OK);
    ASSERT_EQ(write_metadata(memory, coherent_card(), layout), OK);

    EXPECT_EQ(metadata_field(memory, SIGNATURE_BYTES_AT), NEXT_BLOCK_SIGNATURE_BYTES);

    memory.release();
}

TEST(nvidia_gsp_boot_memory, protected_metadata_needs_the_copies_and_is_written_once) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    protected_memory_layout layout = {};
    ASSERT_EQ(lay_out_for_metadata(layout), OK);

    gsp_boot_memory memory;
    EXPECT_EQ(write_metadata(memory, coherent_card(), layout), ERR_BAD_STATE);
    EXPECT_EQ(memory.protected_metadata_address(), static_cast<uint64_t>(0));

    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    ASSERT_EQ(write_metadata(memory, coherent_card(), layout), OK);
    uint64_t metadata_address = memory.protected_metadata_address();

    EXPECT_EQ(write_metadata(memory, coherent_card(), layout), ERR_BAD_STATE);
    EXPECT_EQ(memory.protected_metadata_address(), metadata_address);

    memory.release();
}

TEST(nvidia_gsp_boot_memory, failed_metadata_allocation_keeps_the_copies_and_takes_no_page) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    protected_memory_layout layout = {};
    ASSERT_EQ(lay_out_for_metadata(layout), OK);

    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    uint64_t bootloader_address = memory.bootloader_address();

    dma::device_profile refused_card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);
    uint64_t free_pages_before = drained_free_pages();

    EXPECT_EQ(write_metadata(memory, refused_card, layout), ERR_NO_MEMORY);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.protected_metadata_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.bootloader_address(), bootloader_address);

    memory.release();
}

static uint64_t u64_at(const uint8_t* bytes, uint64_t offset) {
    uint64_t value = 0;
    string::memcpy(&value, bytes + offset, sizeof(value));

    return value;
}

// The bytes at `address`, read through the kernel's mapping of all physical memory
static const uint8_t* bytes_at(uint64_t address) {
    return static_cast<const uint8_t*>(paging::phys_to_virt(address));
}

// LibOS's arguments in a page of their own, as NVIDIA's driver lays them out: regions 32 bytes apart, each an id, an
// address and a size, then a kind byte and a location byte
static constexpr uint64_t LIBOS_ARGUMENTS_BYTES  = 0x1000;
static constexpr uint64_t LIBOS_REGION_BYTES     = 32;
static constexpr uint64_t REGION_ID_AT           = 0;
static constexpr uint64_t REGION_ADDRESS_AT      = 8;
static constexpr uint64_t REGION_SIZE_AT         = 16;
static constexpr uint64_t REGION_KIND_AT         = 24;
static constexpr uint64_t REGION_LOCATION_AT     = 25;
static constexpr uint64_t REGION_PADDING_AT      = 26;
static constexpr uint8_t  CONTIGUOUS_KIND        = 1;
static constexpr uint8_t  SYSTEM_MEMORY_LOCATION = 1;

// NVIDIA's ids for LOGINIT, LOGINTR, LOGRM and RMARGS, each name's characters packed with its first in the highest
// byte used, in the order NVIDIA's driver lists the regions
static constexpr size_t   LIBOS_REGION_COUNT                   = 4;
static constexpr uint64_t LIBOS_REGION_IDS[LIBOS_REGION_COUNT] = {
    0x4C4F47494E4954,
    0x4C4F47494E5452,
    0x4C4F47524D,
    0x524D41524753,
};

// The resource manager's arguments in NVIDIA's layout and the queue layout they give. Every argument after the
// status queue's offset is 0 on NVIDIA's first attempt to boot the first GPU it attaches
static constexpr uint64_t RESOURCE_MANAGER_ARGUMENTS_BYTES = 0x1000;
static constexpr uint64_t QUEUE_MEMORY_ADDRESS_AT          = 0;
static constexpr uint64_t PAGE_TABLE_ENTRIES_AT            = 8;
static constexpr uint64_t COMMAND_QUEUE_OFFSET_AT          = 16;
static constexpr uint64_t STATUS_QUEUE_OFFSET_AT           = 24;
static constexpr uint64_t ZERO_ARGUMENTS_AT                = 32;
static constexpr uint64_t QUEUE_PAGE_TABLE_ENTRIES         = 129;
static constexpr uint64_t COMMAND_QUEUE_OFFSET             = 0x1000;
static constexpr uint64_t STATUS_QUEUE_OFFSET              = 0x41000;

// Each log buffer's 64 KiB, NVIDIA's size in its release build, start with the firmware's write position, then list
// the address of each of its pages
static constexpr uint64_t LOG_BUFFER_BYTES = 0x10000;
static constexpr uint64_t LOG_BUFFER_PAGES = LOG_BUFFER_BYTES / PAGE_BYTES;

namespace drivers::nvidia {

// Reaches the parts the object keeps to itself: their buffers, whose caching the tests check and through whose own
// mapping LibOS's uncached page is read, since the kernel's mapping of all physical memory is cached
struct gsp_boot_memory_test_access {
    static const dma::buffer& log_buffer(const gsp_boot_memory& memory, size_t index) {
        return memory.m_log_buffers[index];
    }

    static const dma::buffer& resource_manager_arguments(const gsp_boot_memory& memory) {
        return memory.m_resource_manager_arguments;
    }

    static const dma::buffer& libos_arguments(const gsp_boot_memory& memory) {
        return memory.m_libos_arguments;
    }

    // Holds every part `write_firmware_arguments` allocates but LibOS's arguments, which it allocates last, as the
    // call holds them when that last allocation fails
    static int32_t hold_all_but_libos_arguments(gsp_boot_memory& memory, const dma::device_profile& device) {
        int32_t rc = memory.allocate_firmware_arguments(device);
        RUN_ELEVATED(dma::free_pages(memory.m_libos_arguments));

        return rc;
    }
};

} // namespace drivers::nvidia

using drivers::nvidia::gsp_boot_memory_test_access;

TEST(nvidia_gsp_boot_memory, firmware_arguments_hold_the_queue_layout_the_log_pages_and_the_regions_libos_maps) {
    gsp_boot_memory memory;
    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);
    ASSERT_EQ(memory.write_firmware_arguments(coherent_card()), OK);

    const uint8_t* arguments = bytes_at(memory.resource_manager_arguments_address());

    uint64_t nonzero_bytes_past_the_queue_layout = 0;
    for (uint64_t offset = ZERO_ARGUMENTS_AT; offset < RESOURCE_MANAGER_ARGUMENTS_BYTES; offset++) {
        nonzero_bytes_past_the_queue_layout += arguments[offset] != 0;
    }

    EXPECT_EQ(u64_at(arguments, QUEUE_MEMORY_ADDRESS_AT), memory.message_queues_address());
    EXPECT_EQ(u64_at(arguments, PAGE_TABLE_ENTRIES_AT), QUEUE_PAGE_TABLE_ENTRIES);
    EXPECT_EQ(u64_at(arguments, COMMAND_QUEUE_OFFSET_AT), COMMAND_QUEUE_OFFSET);
    EXPECT_EQ(u64_at(arguments, STATUS_QUEUE_OFFSET_AT), STATUS_QUEUE_OFFSET);
    EXPECT_EQ(nonzero_bytes_past_the_queue_layout, static_cast<uint64_t>(0));

    for (size_t i = 0; i < FIRMWARE_LOG_COUNT; i++) {
        uint64_t address = memory.log_buffer_address(i);
        const uint8_t* log_buffer = bytes_at(address);

        uint64_t wrong_page_addresses = 0;
        for (uint64_t page = 0; page < LOG_BUFFER_PAGES; page++) {
            uint64_t listed_address = u64_at(log_buffer, (1 + page) * sizeof(uint64_t));
            wrong_page_addresses += listed_address != address + page * PAGE_BYTES;
        }

        uint64_t nonzero_bytes_past_the_list = 0;
        for (uint64_t offset = (1 + LOG_BUFFER_PAGES) * sizeof(uint64_t); offset < LOG_BUFFER_BYTES; offset++) {
            nonzero_bytes_past_the_list += log_buffer[offset] != 0;
        }

        EXPECT_EQ(address % PAGE_BYTES, static_cast<uint64_t>(0));
        EXPECT_EQ(u64_at(log_buffer, 0), static_cast<uint64_t>(0));
        EXPECT_EQ(wrong_page_addresses, static_cast<uint64_t>(0));
        EXPECT_EQ(nonzero_bytes_past_the_list, static_cast<uint64_t>(0));
        EXPECT_TRUE(gsp_boot_memory_test_access::log_buffer(memory, i).cached);
    }

    uint64_t addresses[] = {
        memory.log_buffer_address(0),
        memory.log_buffer_address(1),
        memory.log_buffer_address(2),
        memory.resource_manager_arguments_address(),
        memory.libos_arguments_address(),
    };

    constexpr size_t address_count = sizeof(addresses) / sizeof(addresses[0]);

    uint64_t shared_addresses = 0;
    for (size_t i = 0; i < address_count; i++) {
        for (size_t j = i + 1; j < address_count; j++) {
            shared_addresses += addresses[i] == addresses[j];
        }
    }

    EXPECT_EQ(shared_addresses, static_cast<uint64_t>(0));
    EXPECT_NE(memory.libos_arguments_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.libos_arguments_address() % LIBOS_ARGUMENTS_BYTES, static_cast<uint64_t>(0));
    EXPECT_EQ(memory.log_buffer_address(FIRMWARE_LOG_COUNT), static_cast<uint64_t>(0));
    EXPECT_TRUE(gsp_boot_memory_test_access::resource_manager_arguments(memory).cached);
    EXPECT_FALSE(gsp_boot_memory_test_access::libos_arguments(memory).cached);

    const dma::buffer& libos_page = gsp_boot_memory_test_access::libos_arguments(memory);
    const uint8_t* libos_arguments = reinterpret_cast<const uint8_t*>(libos_page.virt);
    uint64_t listed_bytes[LIBOS_REGION_COUNT] = {
        LOG_BUFFER_BYTES,
        LOG_BUFFER_BYTES,
        LOG_BUFFER_BYTES,
        RESOURCE_MANAGER_ARGUMENTS_BYTES,
    };

    for (size_t i = 0; i < LIBOS_REGION_COUNT; i++) {
        const uint8_t* region = libos_arguments + i * LIBOS_REGION_BYTES;

        uint64_t nonzero_padding_bytes = 0;
        for (uint64_t offset = REGION_PADDING_AT; offset < LIBOS_REGION_BYTES; offset++) {
            nonzero_padding_bytes += region[offset] != 0;
        }

        EXPECT_EQ(u64_at(region, REGION_ID_AT), LIBOS_REGION_IDS[i]);
        EXPECT_EQ(u64_at(region, REGION_ADDRESS_AT), addresses[i]);
        EXPECT_EQ(u64_at(region, REGION_SIZE_AT), listed_bytes[i]);
        EXPECT_EQ(region[REGION_KIND_AT], CONTIGUOUS_KIND);
        EXPECT_EQ(region[REGION_LOCATION_AT], SYSTEM_MEMORY_LOCATION);
        EXPECT_EQ(nonzero_padding_bytes, static_cast<uint64_t>(0));
    }

    uint64_t nonzero_bytes_past_the_regions = 0;
    for (uint64_t offset = LIBOS_REGION_COUNT * LIBOS_REGION_BYTES; offset < LIBOS_ARGUMENTS_BYTES; offset++) {
        nonzero_bytes_past_the_regions += libos_arguments[offset] != 0;
    }

    EXPECT_EQ(nonzero_bytes_past_the_regions, static_cast<uint64_t>(0));

    memory.release();

    EXPECT_EQ(memory.resource_manager_arguments_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.libos_arguments_address(), static_cast<uint64_t>(0));
    for (size_t i = 0; i < FIRMWARE_LOG_COUNT; i++) {
        EXPECT_EQ(memory.log_buffer_address(i), static_cast<uint64_t>(0));
    }
}

TEST(nvidia_gsp_boot_memory, firmware_arguments_are_all_uncached_for_a_card_that_is_not_coherent) {
    dma::device_profile non_coherent_card = dma::device_profile_test_access::build(64, false);

    gsp_boot_memory memory;
    ASSERT_EQ(memory.allocate_message_queues(non_coherent_card), OK);
    ASSERT_EQ(memory.write_firmware_arguments(non_coherent_card), OK);

    for (size_t i = 0; i < FIRMWARE_LOG_COUNT; i++) {
        EXPECT_FALSE(gsp_boot_memory_test_access::log_buffer(memory, i).cached);
    }

    EXPECT_FALSE(gsp_boot_memory_test_access::resource_manager_arguments(memory).cached);
    EXPECT_FALSE(gsp_boot_memory_test_access::libos_arguments(memory).cached);

    memory.release();
}

TEST(nvidia_gsp_boot_memory, message_queues_lay_out_through_the_cpu_mapping_of_their_memory) {
    gsp_boot_memory memory;
    EXPECT_NULL(memory.message_queues_memory());
    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);

    uint64_t queue_memory_address = memory.message_queues_address();
    message_queues queues(memory.message_queues_memory(), queue_memory_address);

    const uint8_t* page_table = bytes_at(queue_memory_address);
    uint64_t last_entry_at = (QUEUE_PAGE_TABLE_ENTRIES - 1) * sizeof(uint64_t);
    uint64_t last_page_address = queue_memory_address + (QUEUE_PAGE_TABLE_ENTRIES - 1) * PAGE_BYTES;
    EXPECT_EQ(u64_at(page_table, 0), queue_memory_address);
    EXPECT_EQ(u64_at(page_table, last_entry_at), last_page_address);

    memory.release();

    EXPECT_NULL(memory.message_queues_memory());
}

TEST(nvidia_gsp_boot_memory, firmware_arguments_need_the_queue_memory_and_are_written_once) {
    gsp_boot_memory memory;
    uint64_t free_pages_before = drained_free_pages();

    EXPECT_EQ(memory.write_firmware_arguments(coherent_card()), ERR_BAD_STATE);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.libos_arguments_address(), static_cast<uint64_t>(0));

    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);
    ASSERT_EQ(memory.write_firmware_arguments(coherent_card()), OK);
    uint64_t libos_arguments_address = memory.libos_arguments_address();
    uint64_t resource_manager_arguments_address = memory.resource_manager_arguments_address();
    uint64_t first_log_buffer_address = memory.log_buffer_address(0);

    EXPECT_EQ(memory.write_firmware_arguments(coherent_card()), ERR_BAD_STATE);
    EXPECT_EQ(memory.libos_arguments_address(), libos_arguments_address);
    EXPECT_EQ(memory.resource_manager_arguments_address(), resource_manager_arguments_address);
    EXPECT_EQ(memory.log_buffer_address(0), first_log_buffer_address);

    memory.release();
}

TEST(nvidia_gsp_boot_memory, failed_first_firmware_arguments_allocation_takes_no_page_and_keeps_the_queue_memory) {
    gsp_boot_memory memory;
    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);
    uint64_t queue_memory_address = memory.message_queues_address();

    dma::device_profile refused_card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);
    uint64_t free_pages_before = drained_free_pages();

    EXPECT_EQ(memory.write_firmware_arguments(refused_card), ERR_NO_MEMORY);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.libos_arguments_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.resource_manager_arguments_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.log_buffer_address(0), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.message_queues_address(), queue_memory_address);

    memory.release();
}

TEST(nvidia_gsp_boot_memory, failed_firmware_arguments_allocation_frees_the_parts_already_held) {
    gsp_boot_memory memory;
    ASSERT_EQ(memory.allocate_message_queues(coherent_card()), OK);
    uint64_t queue_memory_address = memory.message_queues_address();
    uint64_t free_pages_before = drained_free_pages();

    ASSERT_EQ(gsp_boot_memory_test_access::hold_all_but_libos_arguments(memory, coherent_card()), OK);
    dma::device_profile refused_card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);

    EXPECT_EQ(memory.write_firmware_arguments(refused_card), ERR_NO_MEMORY);
    EXPECT_GE(drained_free_pages() + KERNEL_PAGE_TABLE_PAGES, free_pages_before);

    // The page count cannot tell the resource manager's one page from a kept page table, so its address shows it freed
    EXPECT_EQ(memory.resource_manager_arguments_address(), static_cast<uint64_t>(0));
    for (size_t i = 0; i < FIRMWARE_LOG_COUNT; i++) {
        EXPECT_EQ(memory.log_buffer_address(i), static_cast<uint64_t>(0));
    }

    EXPECT_EQ(memory.message_queues_address(), queue_memory_address);

    memory.release();
}
