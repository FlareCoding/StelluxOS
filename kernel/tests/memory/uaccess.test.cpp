#define STLX_TEST_TIER TIER_MM_CORE

#include "stlx_unit_test.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/uaccess.h"
#include "mm/page_quarantine.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/sched_internal.h"
#include "sync/spinlock.h"
#include "common/string.h"

TEST_SUITE(uaccess);

static constexpr size_t PAGE = pmm::PAGE_SIZE;
static constexpr uint32_t PROT_RW = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
static constexpr uint32_t LAZY_ANON =
    mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS | mm::MM_MAP_LAZY;
static constexpr uint32_t EAGER_ANON = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;

static uint64_t g_initial_free_pages = 0;
static sync::spinlock g_masked_lock = sync::SPINLOCK_INIT;

// Runs the calling task under a user address space so copies reach it the
// way a syscall body does, then puts the kernel root back
struct user_space_scope {
    sched::task* self;
    pmm::phys_addr_t saved_root;

    explicit user_space_scope(mm::mm_context* ctx)
        : self(sched::current()), saved_root(self->exec.pt_root) {
        self->exec.mm_ctx = ctx;
        self->exec.pt_root = paging::supervisor_pt_root_for_user_task(ctx->pt_root);
        self->exec.user_pt_root = ctx->pt_root;
        sched::arch_post_switch(self);
    }

    ~user_space_scope() {
        self->exec.mm_ctx = nullptr;
        self->exec.pt_root = saved_root;
        self->exec.user_pt_root = 0;
        sched::arch_post_switch(self);
    }
};

static int32_t uaccess_before_all() {
    page_quarantine::drain();
    g_initial_free_pages = pmm::free_page_count();
    return 0;
}

static int32_t uaccess_after_all() {
    page_quarantine::drain();
    return pmm::free_page_count() == g_initial_free_pages ? 0 : -1;
}

BEFORE_ALL(uaccess, uaccess_before_all);
AFTER_ALL(uaccess, uaccess_after_all);

static uint8_t* page_bytes(mm::mm_context* ctx, uintptr_t user_addr) {
    pmm::phys_addr_t phys = paging::get_physical(user_addr & ~(PAGE - 1), ctx->pt_root);
    if (phys == 0) {
        return nullptr;
    }
    return static_cast<uint8_t*>(paging::phys_to_virt(phys)) + (user_addr & (PAGE - 1));
}

static uintptr_t map_user(mm::mm_context* ctx, size_t pages, uint32_t prot, uint32_t flags) {
    uintptr_t addr = 0;
    if (mm::mm_context_map_anonymous(ctx, 0, pages * PAGE, prot, flags, &addr) != mm::MM_CTX_OK) {
        return 0;
    }
    return addr;
}

// --- copy_to_lazy_page_faults_it_in ---
// Proves: a copy into a never touched lazy page resolves the fault from
// kernel mode, lands the bytes, and leaves neighboring pages absent.

TEST(uaccess, copy_to_lazy_page_faults_it_in) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 2, PROT_RW, LAZY_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));

    {
        user_space_scope scope(ctx);
        alignas(8) uint8_t pattern[64];
        for (size_t i = 0; i < sizeof(pattern); i++) {
            pattern[i] = static_cast<uint8_t>(0xA0 + i);
        }

        EXPECT_EQ(mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(addr + PAGE + 100), pattern, sizeof(pattern)
        ), mm::uaccess::OK);

        uint8_t* landed = page_bytes(ctx, addr + PAGE + 100);
        EXPECT_NOT_NULL(landed);
        if (landed) {
            EXPECT_EQ(string::memcmp(landed, pattern, sizeof(pattern)), 0);
        }
        EXPECT_NULL(page_bytes(ctx, addr));
    }

    mm::mm_context_release(ctx);
}

// --- copy_from_user_reads_mapped_memory ---
// Proves: a copy out of a present page returns exactly its contents.

TEST(uaccess, copy_from_user_reads_mapped_memory) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 1, PROT_RW, EAGER_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));

    uint8_t* backing = page_bytes(ctx, addr + 40);
    ASSERT_NOT_NULL(backing);
    for (size_t i = 0; i < 32; i++) {
        backing[i] = static_cast<uint8_t>(0x10 + i);
    }

    {
        user_space_scope scope(ctx);
        alignas(8) uint8_t out[32];
        string::memset(out, 0, sizeof(out));
        EXPECT_EQ(mm::uaccess::copy_from_user(
            out, reinterpret_cast<const void*>(addr + 40), sizeof(out)
        ), mm::uaccess::OK);
        EXPECT_EQ(string::memcmp(out, backing, sizeof(out)), 0);
    }

    mm::mm_context_release(ctx);
}

// --- write_to_read_only_page_reports_fault ---
// Proves: the hardware permission check stands in for the old VMA walk,
// a write into a read-only page fails while a read of it succeeds.

TEST(uaccess, write_to_read_only_page_reports_fault) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 1, mm::MM_PROT_READ, EAGER_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));

    {
        user_space_scope scope(ctx);
        alignas(8) uint8_t buf[16];
        string::memset(buf, 0x5A, sizeof(buf));
        EXPECT_EQ(mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(addr), buf, sizeof(buf)
        ), mm::uaccess::ERR_FAULT);
        EXPECT_EQ(mm::uaccess::copy_from_user(
            buf, reinterpret_cast<const void*>(addr), sizeof(buf)
        ), mm::uaccess::OK);
        EXPECT_EQ(buf[0], static_cast<uint8_t>(0));
    }

    mm::mm_context_release(ctx);
}

// --- unmapped_range_reports_fault ---
// Proves: a copy touching an address with no mapping behind it fails in
// both directions instead of faulting the kernel.

TEST(uaccess, unmapped_range_reports_fault) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 1, PROT_RW, EAGER_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));
    uintptr_t hole = addr + 4 * PAGE;

    {
        user_space_scope scope(ctx);
        alignas(8) uint8_t buf[16];
        string::memset(buf, 0x33, sizeof(buf));
        EXPECT_EQ(mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(hole), buf, sizeof(buf)
        ), mm::uaccess::ERR_FAULT);
        EXPECT_EQ(mm::uaccess::copy_from_user(
            buf, reinterpret_cast<const void*>(hole), sizeof(buf)
        ), mm::uaccess::ERR_FAULT);
    }

    mm::mm_context_release(ctx);
}

// --- copy_stops_at_the_edge_of_the_mapping ---
// Proves: a copy that runs off the end of a mapping delivers the bytes
// before the edge and reports the failure for the rest.

TEST(uaccess, copy_stops_at_the_edge_of_the_mapping) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 2, PROT_RW, LAZY_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));
    uintptr_t edge = addr + 2 * PAGE;

    {
        user_space_scope scope(ctx);
        alignas(8) uint8_t pattern[32];
        string::memset(pattern, 0x7E, sizeof(pattern));
        EXPECT_EQ(mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(edge - 16), pattern, sizeof(pattern)
        ), mm::uaccess::ERR_FAULT);

        uint8_t* tail = page_bytes(ctx, edge - 16);
        EXPECT_NOT_NULL(tail);
        if (tail) {
            EXPECT_EQ(string::memcmp(tail, pattern, 16), 0);
        }
    }

    mm::mm_context_release(ctx);
}

// --- kernel_addresses_are_rejected_before_any_access ---
// Proves: a user pointer that points into the kernel half is refused by
// the bound check, since ring 0 could otherwise reach it.

TEST(uaccess, kernel_addresses_are_rejected_before_any_access) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    {
        user_space_scope scope(ctx);
        alignas(8) uint8_t buf[16];
        string::memset(buf, 0x11, sizeof(buf));
        uint8_t kernel_target[16];
        string::memset(kernel_target, 0x22, sizeof(kernel_target));

        EXPECT_EQ(mm::uaccess::copy_to_user(kernel_target, buf, sizeof(buf)), mm::uaccess::ERR_FAULT);
        EXPECT_EQ(kernel_target[0], static_cast<uint8_t>(0x22));
        EXPECT_EQ(mm::uaccess::copy_from_user(buf, kernel_target, sizeof(buf)), mm::uaccess::ERR_FAULT);
        EXPECT_EQ(buf[0], static_cast<uint8_t>(0x11));
        EXPECT_EQ(mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(mm::USER_STACK_TOP - 8), buf, 16
        ), mm::uaccess::ERR_FAULT);
    }

    mm::mm_context_release(ctx);
}

// --- cstr_copy_stops_at_the_terminator ---
// Proves: a string ending right before an unmapped page copies without
// touching that page, and one without a terminator there fails.

TEST(uaccess, cstr_copy_stops_at_the_terminator) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 1, PROT_RW, EAGER_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));

    uint8_t* last = page_bytes(ctx, addr + PAGE - 4);
    ASSERT_NOT_NULL(last);
    string::memcpy(last, "abc", 4);

    {
        user_space_scope scope(ctx);
        char out[64];
        string::memset(out, 'z', sizeof(out));
        EXPECT_EQ(mm::uaccess::copy_cstr_from_user(
            out, sizeof(out), reinterpret_cast<const char*>(addr + PAGE - 4)
        ), mm::uaccess::OK);
        EXPECT_EQ(string::strcmp(out, "abc"), 0);

        string::memset(last, 'x', 4);
        EXPECT_EQ(mm::uaccess::copy_cstr_from_user(
            out, sizeof(out), reinterpret_cast<const char*>(addr + PAGE - 4)
        ), mm::uaccess::ERR_FAULT);
    }

    mm::mm_context_release(ctx);
}

// --- nonblock_copy_lands_in_a_lazy_page ---
// Proves: the interrupt-context copy still faults lazy pages in under the
// tried lock and delivers its bytes through the same primitive.

TEST(uaccess, nonblock_copy_lands_in_a_lazy_page) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 1, PROT_RW, LAZY_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));

    {
        user_space_scope scope(ctx);
        alignas(8) uint8_t pattern[24];
        string::memset(pattern, 0xC3, sizeof(pattern));
        EXPECT_EQ(mm::uaccess::copy_to_user_nonblock(
            reinterpret_cast<void*>(addr + 8), pattern, sizeof(pattern)
        ), mm::uaccess::OK);

        uint8_t* landed = page_bytes(ctx, addr + 8);
        EXPECT_NOT_NULL(landed);
        if (landed) {
            EXPECT_EQ(string::memcmp(landed, pattern, sizeof(pattern)), 0);
        }
    }

    mm::mm_context_release(ctx);
}

// --- load_u32_reads_a_word_and_rejects_bad_addresses ---
// Proves: the single-access load returns the word behind a present page and
// refuses a missing page or a misaligned address without touching either.

TEST(uaccess, load_u32_reads_a_word_and_rejects_bad_addresses) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 1, PROT_RW, EAGER_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));

    uint8_t* backing = page_bytes(ctx, addr + 64);
    ASSERT_NOT_NULL(backing);
    uint32_t stored = 0x12345678u;
    string::memcpy(backing, &stored, sizeof(stored));

    {
        user_space_scope scope(ctx);
        uint32_t value = 0;
        EXPECT_EQ(mm::uaccess::load_u32_from_user(
            reinterpret_cast<const uint32_t*>(addr + 64), &value
        ), mm::uaccess::OK);
        EXPECT_EQ(value, stored);

        EXPECT_EQ(mm::uaccess::load_u32_from_user(
            reinterpret_cast<const uint32_t*>(addr + 4 * PAGE), &value
        ), mm::uaccess::ERR_FAULT);
        EXPECT_EQ(mm::uaccess::load_u32_from_user(
            reinterpret_cast<const uint32_t*>(addr + 66), &value
        ), mm::uaccess::ERR_INVAL);
    }

    mm::mm_context_release(ctx);
}

// --- masked_context_never_faults_a_page_in ---
// Proves: with interrupts off a missing page reports a fault instead of
// sleeping on the address-space lock, and succeeds once they are back on.

TEST(uaccess, masked_context_never_faults_a_page_in) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_user(ctx, 1, PROT_RW, LAZY_ANON);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));

    {
        user_space_scope scope(ctx);
        uint32_t value = 0xFFFFFFFFu;

        sync::irq_state irq = sync::spin_lock_irqsave(g_masked_lock);
        int32_t masked_rc = mm::uaccess::load_u32_from_user(
            reinterpret_cast<const uint32_t*>(addr), &value);
        sync::spin_unlock_irqrestore(g_masked_lock, irq);

        EXPECT_EQ(masked_rc, mm::uaccess::ERR_FAULT);
        EXPECT_NULL(page_bytes(ctx, addr));

        EXPECT_EQ(mm::uaccess::load_u32_from_user(
            reinterpret_cast<const uint32_t*>(addr), &value
        ), mm::uaccess::OK);
        EXPECT_EQ(value, static_cast<uint32_t>(0));
        EXPECT_NOT_NULL(page_bytes(ctx, addr));
    }

    mm::mm_context_release(ctx);
}
