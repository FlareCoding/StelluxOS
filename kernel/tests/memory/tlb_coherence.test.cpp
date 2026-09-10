#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "mm/uaccess.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"

using test_helpers::spin_wait;

TEST_SUITE(tlb_coherence);

constexpr uint8_t FIRST_PATTERN  = 0x11;
constexpr uint8_t SECOND_PATTERN = 0x22;

// Well above the per-page invalidation ceiling, so the full flush path runs
constexpr size_t LARGE_RANGE_PAGES = 2 * paging::FULL_FLUSH_PAGE_THRESHOLD;

// A reader on another CPU caches the translation of one page, waits while
// this CPU remaps the address to a different frame, then reads again. It
// sees the second frame only if the remap was flushed on every CPU.
static volatile uint8_t* g_page = nullptr;
static sync::atomic<uint32_t> g_first_read_done;
static sync::atomic<uint32_t> g_go;
static sync::atomic<uint32_t> g_done;
static sync::atomic<uint32_t> g_first_value;
static sync::atomic<uint32_t> g_second_value;

static void reader_fn(void*) {
    g_first_value.store_release(*g_page);
    g_first_read_done.store_release(1);

    while (!g_go.load_acquire()) {
    }

    g_second_value.store_release(*g_page);
    g_done.store_release(1);
    sched::exit(0);
}

static uint32_t pick_other_online_cpu() {
    uint32_t self = percpu::current_cpu_id();
    for (uint32_t cpu = 0; cpu < smp::cpu_count(); cpu++) {
        smp::cpu_info* info = smp::get_cpu_info(cpu);
        if (cpu != self && info->state.load_acquire() == smp::CPU_ONLINE) {
            return cpu;
        }
    }

    return self;
}

static void reset_reader_state() {
    g_first_read_done.store_relaxed(0);
    g_go.store_relaxed(0);
    g_done.store_relaxed(0);
    g_first_value.store_relaxed(0);
    g_second_value.store_relaxed(0);
}

// Runs the scenario with `flush` as the operation under test. `flush` must
// invalidate `va` on every CPU for the reader to observe the second frame.
static void remap_is_seen_by_other_cpu(void (*flush)(uintptr_t va)) {
    uint32_t reader_cpu = pick_other_online_cpu();
    if (reader_cpu == percpu::current_cpu_id()) {
        return;
    }

    reset_reader_state();

    uintptr_t va = 0;
    pmm::phys_addr_t first_frame = 0;
    int32_t rc = vmm::OK;
    RUN_ELEVATED({
        rc = vmm::alloc(1, paging::PAGE_USER_RW, vmm::ALLOC_ZERO, kva::tag::generic, va);
        if (rc == vmm::OK) {
            first_frame = paging::get_physical(va, paging::get_kernel_pt_root());
        }
    });
    ASSERT_EQ(rc, vmm::OK);
    ASSERT_NE(first_frame, 0u);

    g_page = reinterpret_cast<volatile uint8_t*>(va);
    *g_page = FIRST_PATTERN;

    bool spawned = false;
    RUN_ELEVATED({
        sched::task* reader = sched::create_kernel_task(reader_fn, nullptr, "tlb_reader");
        if (reader) {
            sched::enqueue_on(reader, reader_cpu);
            spawned = true;
        }
    });
    ASSERT_TRUE(spawned);

    ASSERT_TRUE(spin_wait(g_first_read_done));
    EXPECT_EQ(g_first_value.load_acquire(), FIRST_PATTERN);

    // Same address, different frame, then the flush under test
    pmm::phys_addr_t second_frame = 0;
    RUN_ELEVATED({
        pmm::phys_addr_t root = paging::get_kernel_pt_root();
        second_frame = pmm::alloc_page();
        if (second_frame != 0) {
            string::memset(paging::phys_to_virt(second_frame), SECOND_PATTERN, paging::PAGE_SIZE_4KB);
            paging::unmap_page(va, root);
            rc = paging::map_page(va, second_frame, paging::PAGE_USER_RW, root);
            flush(va);
        }
    });
    ASSERT_NE(second_frame, 0u);
    ASSERT_EQ(rc, paging::OK);

    g_go.store_release(1);
    ASSERT_TRUE(spin_wait(g_done));
    EXPECT_EQ(g_second_value.load_acquire(), SECOND_PATTERN);

    RUN_ELEVATED({
        vmm::free(va);
        pmm::free_page(first_frame);
    });
}

static void flush_one_page(uintptr_t va) {
    paging::flush_tlb_page(va);
}

static void flush_large_range(uintptr_t va) {
    paging::flush_tlb_range(va, va + LARGE_RANGE_PAGES * paging::PAGE_SIZE_4KB);
}

TEST(tlb_coherence, page_flush_reaches_other_cpu) {
    remap_is_seen_by_other_cpu(flush_one_page);
}

TEST(tlb_coherence, large_range_flush_reaches_other_cpu) {
    remap_is_seen_by_other_cpu(flush_large_range);
}

// A reader on another CPU caches a user translation through a kernel copy,
// then copies again after this CPU unmapped the page. It faults only if the
// unmap flushed every CPU, otherwise its stale entry still reads the frame.
static mm::mm_context* g_user_ctx = nullptr;
static uintptr_t g_user_va = 0;
static sync::atomic<int32_t> g_first_rc;
static sync::atomic<int32_t> g_second_rc;

static void user_reader_fn(void*) {
    RUN_ELEVATED({
        test_helpers::user_space_scope scope(g_user_ctx);
        uint8_t value = 0;

        g_first_rc.store_release(mm::uaccess::copy_from_user(
            &value, reinterpret_cast<const void*>(g_user_va), sizeof(value)));
        g_first_value.store_release(value);
        g_first_read_done.store_release(1);

        while (!g_go.load_acquire()) {
        }

        g_second_rc.store_release(mm::uaccess::copy_from_user(
            &value, reinterpret_cast<const void*>(g_user_va), sizeof(value)));
    });

    g_done.store_release(1);
    sched::exit(0);
}

TEST(tlb_coherence, user_unmap_reaches_other_cpu) {
    uint32_t reader_cpu = pick_other_online_cpu();
    if (reader_cpu == percpu::current_cpu_id()) {
        return;
    }

    reset_reader_state();
    g_first_rc.store_relaxed(-1);
    g_second_rc.store_relaxed(-1);

    mm::mm_context* ctx = nullptr;
    uintptr_t va = 0;
    int32_t rc = mm::MM_CTX_OK;
    RUN_ELEVATED({
        ctx = mm::mm_context_create();
        if (ctx) {
            rc = mm::mm_context_map_anonymous(
                ctx, 0, paging::PAGE_SIZE_4KB, mm::MM_PROT_READ | mm::MM_PROT_WRITE,
                mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS, &va);
        }
        if (rc == mm::MM_CTX_OK && va != 0) {
            pmm::phys_addr_t frame = paging::get_physical(va, ctx->pt_root);
            string::memset(paging::phys_to_virt(frame), FIRST_PATTERN, paging::PAGE_SIZE_4KB);
        }
    });
    ASSERT_NOT_NULL(ctx);
    ASSERT_EQ(rc, mm::MM_CTX_OK);
    ASSERT_NE(va, 0u);

    g_user_ctx = ctx;
    g_user_va = va;

    bool spawned = false;
    RUN_ELEVATED({
        sched::task* reader = sched::create_kernel_task(user_reader_fn, nullptr, "tlb_ureader");
        if (reader) {
            sched::enqueue_on(reader, reader_cpu);
            spawned = true;
        }
    });
    ASSERT_TRUE(spawned);

    ASSERT_TRUE(spin_wait(g_first_read_done));
    EXPECT_EQ(g_first_rc.load_acquire(), mm::uaccess::OK);
    EXPECT_EQ(g_first_value.load_acquire(), FIRST_PATTERN);

    RUN_ELEVATED({
        rc = mm::mm_context_unmap(ctx, va, paging::PAGE_SIZE_4KB);
    });
    EXPECT_EQ(rc, mm::MM_CTX_OK);

    g_go.store_release(1);
    ASSERT_TRUE(spin_wait(g_done));
    EXPECT_EQ(g_second_rc.load_acquire(), mm::uaccess::ERR_FAULT);

    RUN_ELEVATED({
        mm::mm_context_release(ctx);
    });
}
