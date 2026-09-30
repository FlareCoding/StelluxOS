#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "exec/elf.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "resource/providers/proc_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(startup_stack);

static constexpr const char* PROGRAM_PATH = "/bin/hello";
static constexpr uint64_t AT_NULL = 0;
static constexpr uint64_t AT_RANDOM = 25;
static constexpr size_t RANDOM_SEED_BYTES = 16;

// Bounds each walk, so a malformed layout fails the test instead of looping
static constexpr size_t MAX_WORDS_SCANNED = 256;

// A task that has not run yet is read through its own page tables
static uint64_t read_user_word(sched::task* t, uintptr_t va) {
    uintptr_t page_va = pmm::page_align_down(va);
    uint64_t word = 0;
    RUN_ELEVATED({
        pmm::phys_addr_t frame = paging::get_physical(page_va, t->exec.user_pt_root);
        if (frame) {
            auto* page = static_cast<uint8_t*>(paging::phys_to_virt(frame));
            string::memcpy(&word, page + (va - page_va), sizeof(word));
        }
    });

    return word;
}

// Walks past argc, argv, and envp to the auxiliary vector and returns the value of `type`, or 0
static uint64_t find_auxv_value(sched::task* t, uint64_t type) {
    uintptr_t cursor = t->exec.task_stack_top;
    uint64_t argc = read_user_word(t, cursor);
    cursor += (1 + argc + 1) * sizeof(uint64_t);

    for (size_t i = 0; i < MAX_WORDS_SCANNED && read_user_word(t, cursor) != 0; i++) {
        cursor += sizeof(uint64_t);
    }

    cursor += sizeof(uint64_t);
    for (size_t i = 0; i < MAX_WORDS_SCANNED; i++) {
        uint64_t entry_type = read_user_word(t, cursor);
        if (entry_type == AT_NULL) {
            return 0;
        }

        if (entry_type == type) {
            return read_user_word(t, cursor + sizeof(uint64_t));
        }

        cursor += 2 * sizeof(uint64_t);
    }

    return 0;
}

static sched::task* create_unstarted_program() {
    exec::loaded_image loaded;
    if (exec::load_elf(PROGRAM_PATH, &loaded) != exec::OK) {
        return nullptr;
    }

    return sched::create_user_task(&loaded, PROGRAM_PATH);
}

TEST(startup_stack, every_program_gets_its_own_random_seed) {
    sched::task* first = create_unstarted_program();
    ASSERT_NOT_NULL(first);
    sched::task* second = create_unstarted_program();
    ASSERT_NOT_NULL(second);

    uint64_t first_seed_va = find_auxv_value(first, AT_RANDOM);
    uint64_t second_seed_va = find_auxv_value(second, AT_RANDOM);
    EXPECT_GT(first_seed_va, first->exec.task_stack_top);
    EXPECT_LE(first_seed_va + RANDOM_SEED_BYTES, mm::USER_STACK_TOP);

    uint64_t first_seed[] = {
        read_user_word(first, first_seed_va),
        read_user_word(first, first_seed_va + sizeof(uint64_t)),
    };

    uint64_t second_seed[] = {
        read_user_word(second, second_seed_va),
        read_user_word(second, second_seed_va + sizeof(uint64_t)),
    };

    EXPECT_NE(string::memcmp(first_seed, second_seed, RANDOM_SEED_BYTES), 0);

    resource::proc_provider::destroy_unstarted_task(first);
    resource::proc_provider::destroy_unstarted_task(second);
}
