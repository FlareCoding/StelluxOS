#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "common/string.h"
#include "common/logging.h"

TEST_SUITE(heap_test);

namespace {

uint64_t g_initial_free_pages = 0;

int32_t heap_before_all() {
    g_initial_free_pages = pmm::free_page_count();
    if (g_initial_free_pages < 256) {
        log::error("heap tests: insufficient free pages (%lu)", g_initial_free_pages);
        return -1;
    }
    return 0;
}

int32_t heap_after_all() {
    uint64_t final_free = pmm::free_page_count();
    if (final_free != g_initial_free_pages) {
        log::error("heap tests: leak detected, started=%lu ended=%lu delta=%ld",
                   g_initial_free_pages, final_free,
                   static_cast<int64_t>(final_free) - static_cast<int64_t>(g_initial_free_pages));
    }
    return 0;
}

} // namespace

BEFORE_ALL(heap_test, heap_before_all);
AFTER_ALL(heap_test, heap_after_all);

TEST(heap_test, kalloc_free_basic) {
    void* p = heap::kalloc(64);
    ASSERT_NOT_NULL(p);
    EXPECT_ALIGNED(reinterpret_cast<uintptr_t>(p), 16);
    ASSERT_EQ(heap::kfree(p), heap::OK);
}

TEST(heap_test, kzalloc_returns_zeroed) {
    void* p = heap::kzalloc(128);
    ASSERT_NOT_NULL(p);
    auto* bytes = static_cast<uint8_t*>(p);
    bool all_zero = true;
    for (size_t i = 0; i < 128; i++) {
        if (bytes[i] != 0) { all_zero = false; break; }
    }
    EXPECT_TRUE(all_zero);
    heap::kfree(p);
}

TEST(heap_test, kalloc_size_class_16) {
    void* p = heap::kalloc(1);
    ASSERT_NOT_NULL(p);
    auto* val = static_cast<uint64_t*>(p);
    *val = 0xCAFEBABEULL;
    EXPECT_EQ(*val, 0xCAFEBABEULL);
    heap::kfree(p);
}

TEST(heap_test, kalloc_size_class_32) {
    void* p = heap::kalloc(17);
    ASSERT_NOT_NULL(p);
    string::memset(p, 0xAA, 32);
    heap::kfree(p);
}

TEST(heap_test, kalloc_size_class_64) {
    void* p = heap::kalloc(33);
    ASSERT_NOT_NULL(p);
    string::memset(p, 0xBB, 64);
    heap::kfree(p);
}

TEST(heap_test, kalloc_size_class_256) {
    void* p = heap::kalloc(200);
    ASSERT_NOT_NULL(p);
    string::memset(p, 0xCC, 200);
    heap::kfree(p);
}

TEST(heap_test, kalloc_size_class_2048) {
    void* p = heap::kalloc(2048);
    ASSERT_NOT_NULL(p);
    string::memset(p, 0xDD, 2048);
    heap::kfree(p);
}

TEST(heap_test, kalloc_large_4096) {
    void* p = heap::kalloc(4096);
    ASSERT_NOT_NULL(p);
    string::memset(p, 0xEE, 4096);
    ASSERT_EQ(heap::kfree(p), heap::OK);
}

TEST(heap_test, kalloc_large_8192) {
    void* p = heap::kalloc(8192);
    ASSERT_NOT_NULL(p);
    string::memset(p, 0xFF, 8192);
    ASSERT_EQ(heap::kfree(p), heap::OK);
}

TEST(heap_test, kalloc_many_free_fifo) {
    constexpr size_t N = 32;
    void* ptrs[N];
    for (size_t i = 0; i < N; i++) {
        ptrs[i] = heap::kalloc(64);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    for (size_t i = 0; i < N; i++) {
        EXPECT_EQ(heap::kfree(ptrs[i]), heap::OK);
    }
}

TEST(heap_test, kalloc_many_free_lifo) {
    constexpr size_t N = 32;
    void* ptrs[N];
    for (size_t i = 0; i < N; i++) {
        ptrs[i] = heap::kalloc(128);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    for (size_t i = N; i > 0; i--) {
        EXPECT_EQ(heap::kfree(ptrs[i - 1]), heap::OK);
    }
}

TEST(heap_test, kalloc_free_no_major_leak) {
    uint64_t before = pmm::free_page_count();

    constexpr size_t N = 16;
    void* ptrs[N];
    for (size_t i = 0; i < N; i++) {
        ptrs[i] = heap::kalloc(64);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    for (size_t i = 0; i < N; i++) {
        heap::kfree(ptrs[i]);
    }

    uint64_t after = pmm::free_page_count();
    int64_t delta = static_cast<int64_t>(after) - static_cast<int64_t>(before);
    EXPECT_GE(delta, static_cast<int64_t>(-4));
}

TEST(heap_test, typed_helper_kalloc_new) {
    struct test_obj {
        uint32_t a;
        uint32_t b;
        uint64_t c;
    };
    auto* obj = heap::kalloc_new<test_obj>();
    ASSERT_NOT_NULL(obj);
    EXPECT_EQ(obj->a, static_cast<uint32_t>(0));
    EXPECT_EQ(obj->b, static_cast<uint32_t>(0));
    EXPECT_EQ(obj->c, static_cast<uint64_t>(0));
    obj->a = 42;
    obj->c = 0xDEADULL;
    heap::kfree_delete(obj);
}

TEST(heap_test, ualloc_smoke) {
    void* p = heap::ualloc(64);
    ASSERT_NOT_NULL(p);
    auto* val = static_cast<uint64_t*>(p);
    *val = 0xBEEFULL;
    EXPECT_EQ(*val, 0xBEEFULL);
    ASSERT_EQ(heap::ufree(p), heap::OK);
}

TEST(heap_test, uzalloc_returns_zeroed) {
    void* p = heap::uzalloc(128);
    ASSERT_NOT_NULL(p);
    auto* bytes = static_cast<uint8_t*>(p);
    bool all_zero = true;
    for (size_t i = 0; i < 128; i++) {
        if (bytes[i] != 0) { all_zero = false; break; }
    }
    EXPECT_TRUE(all_zero);
    heap::ufree(p);
}

TEST(heap_test, mixed_sizes) {
    void* a = heap::kalloc(16);
    void* b = heap::kalloc(64);
    void* c = heap::kalloc(256);
    void* d = heap::kalloc(1024);
    void* e = heap::kalloc(2048);

    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(d);
    ASSERT_NOT_NULL(e);

    EXPECT_NE(reinterpret_cast<uintptr_t>(a), reinterpret_cast<uintptr_t>(b));
    EXPECT_NE(reinterpret_cast<uintptr_t>(b), reinterpret_cast<uintptr_t>(c));

    heap::kfree(c);
    heap::kfree(a);
    heap::kfree(e);
    heap::kfree(b);
    heap::kfree(d);
}

TEST(heap_test, stress_alloc_free) {
    // Warm up all size classes to amortize slab and KVA node pool overhead
    for (uint8_t c = 0; c < 8; c++) {
        size_t sz = static_cast<size_t>(c + 1) * 16;
        void* w = heap::kalloc(sz);
        heap::kfree(w);
        w = heap::kalloc(sz);
        heap::kfree(w);
    }

    uint64_t before = pmm::free_page_count();

    constexpr size_t N = 64;
    void* ptrs[N];
    for (size_t i = 0; i < N; i++) {
        size_t size = ((i % 8) + 1) * 16;
        ptrs[i] = heap::kalloc(size);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    for (size_t i = 0; i < N; i++) {
        EXPECT_EQ(heap::kfree(ptrs[i]), heap::OK);
    }

    uint64_t after = pmm::free_page_count();
    int64_t delta = static_cast<int64_t>(after) - static_cast<int64_t>(before);
    EXPECT_GE(delta, static_cast<int64_t>(-8));
}
