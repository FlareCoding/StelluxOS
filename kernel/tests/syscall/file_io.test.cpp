#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_io.h"
#include "syscall/handlers/sys_mmap.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/uaccess.h"
#include "common/string.h"
#include "percpu/percpu.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "sync/atomic.h"

TEST_SUITE(file_io);

using test_helpers::user_space_scope;

struct file_worker;

// One call a worker makes per round, returning whether it moved every byte it asked for and passed its check
using file_worker_call = bool (*)(file_worker& worker, uint64_t fd, uint32_t round);

struct file_worker_job {
    file_worker_call call;
    const char*      path;       // opened by the worker with `open_flags`
    uint32_t         open_flags;
    mm::mm_context*  ctx;        // the address space the worker's calls run in
    uintptr_t        bytes;      // the user buffer the calls move bytes from or to
    size_t           size;       // the bytes each call asks for
    uint32_t         rounds;     // calls to make, or 0 to call until `should_stop`
};

struct file_worker {
    file_worker_job        job;
    sync::atomic<uint32_t> should_stop;
    sync::atomic<uint64_t> moved;    // bytes the appends returned
    sync::atomic<uint32_t> failures; // calls that fell short or failed their check
    sync::atomic<uint32_t> done;
};

static constexpr size_t PAGE = pmm::PAGE_SIZE;
static constexpr uint32_t PROT_RW = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
static constexpr uint32_t EAGER_ANON = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;
static constexpr uint32_t LAZY_ANON = EAGER_ANON | mm::MM_MAP_LAZY;
static constexpr uint32_t CREATE_RW = fs::O_CREAT | fs::O_TRUNC | fs::O_RDWR;
static constexpr uint32_t OPEN_APPEND = fs::O_WRONLY | fs::O_APPEND;
static constexpr uint64_t PROT_READ_ARG = 0x1;
static constexpr uint64_t MAP_SHARED_ARG = 0x01;

static constexpr uint32_t REMAP_ROUNDS = 400;
static constexpr uint32_t LAZY_READ_ROUNDS = 400;
static constexpr size_t RECORD_BYTES = 16 * PAGE;
static constexpr uint32_t RECORD_ROUNDS = 300;
static constexpr size_t APPEND_RECORD_BYTES = 10 * 1024;
static constexpr uint32_t APPEND_ROUNDS = 64;
static constexpr uint32_t SPLIT_APPEND_ROUNDS = 64;

static file_worker g_first_worker;
static file_worker g_second_worker;

static uint8_t pattern_byte(size_t i) {
    return static_cast<uint8_t>(i * 13 + 1);
}

// The kernel's view of the user byte at `addr`, null while its page is absent
static uint8_t* user_byte(mm::mm_context* ctx, uintptr_t addr) {
    pmm::phys_addr_t phys = paging::get_physical(addr & ~(PAGE - 1), ctx->pt_root);
    return phys ? static_cast<uint8_t*>(paging::phys_to_virt(phys)) + (addr & (PAGE - 1)) : nullptr;
}

static uintptr_t map_user(mm::mm_context* ctx, size_t pages, uint32_t flags) {
    uintptr_t addr = 0;
    if (mm::mm_context_map_anonymous(ctx, 0, pages * PAGE, PROT_RW, flags, &addr) != mm::MM_CTX_OK) {
        return 0;
    }

    return addr;
}

static void fill_user(mm::mm_context* ctx, uintptr_t addr, size_t len, uint8_t value) {
    for (size_t i = 0; i < len; i++) {
        *user_byte(ctx, addr + i) = value;
    }
}

static void fill_user_with_pattern(mm::mm_context* ctx, uintptr_t addr, size_t len) {
    for (size_t i = 0; i < len; i++) {
        *user_byte(ctx, addr + i) = pattern_byte(i);
    }
}

// Whether the `len` user bytes at `addr` hold the pattern from its byte `first` on
static bool user_holds_pattern(mm::mm_context* ctx, uintptr_t addr, size_t len, size_t first) {
    for (size_t i = 0; i < len; i++) {
        uint8_t* byte = user_byte(ctx, addr + i);
        if (!byte || *byte != pattern_byte(first + i)) {
            return false;
        }
    }

    return true;
}

// Resolves each page once rather than each byte, since a reader checks a whole record every round
static bool user_holds_one_value(mm::mm_context* ctx, uintptr_t addr, size_t len) {
    uint8_t first = *user_byte(ctx, addr);
    size_t span = 0;
    for (size_t done = 0; done < len; done += span) {
        uint8_t* bytes = user_byte(ctx, addr + done);
        span = PAGE - ((addr + done) & (PAGE - 1));
        if (span > len - done) {
            span = len - done;
        }

        for (size_t i = 0; i < span; i++) {
            if (bytes[i] != first) {
                return false;
            }
        }
    }

    return true;
}

static bool file_holds_one_value(resource::handle_t h, uint64_t offset, size_t len) {
    sched::task* task = sched::current();
    uint8_t chunk[256];
    uint8_t first = 0;
    for (size_t done = 0; done < len; done += sizeof(chunk)) {
        size_t chunk_len = len - done < sizeof(chunk) ? len - done : sizeof(chunk);
        if (resource::read_at(task, h, chunk, chunk_len, offset + done) != static_cast<ssize_t>(chunk_len)) {
            return false;
        }

        if (done == 0) {
            first = chunk[0];
        }

        for (size_t i = 0; i < chunk_len; i++) {
            if (chunk[i] != first) {
                return false;
            }
        }
    }

    return true;
}

// Opens `path` for the calling task holding `len` pattern bytes, or returns -1
static resource::handle_t create_pattern_file(const char* path, size_t len) {
    sched::task* task = sched::current();
    resource::handle_t h = -1;
    if (resource::open(task, path, CREATE_RW, &h) != resource::OK) {
        return -1;
    }

    uint8_t chunk[256];
    for (size_t done = 0; done < len; done += sizeof(chunk)) {
        size_t chunk_len = len - done < sizeof(chunk) ? len - done : sizeof(chunk);
        for (size_t i = 0; i < chunk_len; i++) {
            chunk[i] = pattern_byte(done + i);
        }

        if (resource::write_at(task, h, chunk, chunk_len, done) != static_cast<ssize_t>(chunk_len)) {
            (void)resource::close(task, h);
            return -1;
        }
    }

    return h;
}

static void remove_file(resource::handle_t h, const char* path) {
    (void)resource::close(sched::current(), h);
    (void)fs::unlink(path);
}

static int64_t file_size(resource::handle_t h) {
    return sys_lseek(static_cast<uint64_t>(h), 0, fs::SEEK_END, 0, 0, 0);
}

static bool map_and_unmap_the_file(file_worker& worker, uint64_t fd, uint32_t) {
    int64_t addr = sys_mmap(0, worker.job.size, PROT_READ_ARG, MAP_SHARED_ARG, fd, 0);
    return addr > 0 && sys_munmap(static_cast<uint64_t>(addr), worker.job.size, 0, 0, 0, 0) == 0;
}

static bool read_into_a_lazy_buffer(file_worker& worker, uint64_t fd, uint32_t) {
    uintptr_t lazy = map_user(worker.job.ctx, worker.job.size / PAGE, LAZY_ANON);
    int64_t n = sys_pread64(fd, lazy, worker.job.size, 0, 0, 0);
    (void)mm::mm_context_unmap(worker.job.ctx, lazy, worker.job.size);

    return n == static_cast<int64_t>(worker.job.size);
}

// Writes the first or the second of the two records at `bytes`, in turn
static bool write_records_in_turn(file_worker& worker, uint64_t fd, uint32_t round) {
    uintptr_t record = worker.job.bytes + (round % 2) * worker.job.size;
    return sys_pwrite64(fd, record, worker.job.size, 0, 0, 0) == static_cast<int64_t>(worker.job.size);
}

static bool read_a_whole_record(file_worker& worker, uint64_t fd, uint32_t) {
    int64_t n = sys_pread64(fd, worker.job.bytes, worker.job.size, 0, 0, 0);
    return n == static_cast<int64_t>(worker.job.size) &&
           user_holds_one_value(worker.job.ctx, worker.job.bytes, worker.job.size);
}

static bool append_the_bytes(file_worker& worker, uint64_t fd, uint32_t) {
    int64_t n = sys_write(fd, worker.job.bytes, worker.job.size, 0, 0, 0);
    if (n > 0) {
        worker.moved.fetch_add_relaxed(static_cast<uint64_t>(n));
    }

    return n == static_cast<int64_t>(worker.job.size);
}

// Appends two pages from a fresh buffer whose second page was never touched, so a fault splits the append
static bool append_a_half_lazy_buffer(file_worker& worker, uint64_t fd, uint32_t) {
    uintptr_t buffer = map_user(worker.job.ctx, 2, LAZY_ANON);
    uint8_t first_byte = 'A';
    (void)mm::uaccess::copy_to_user(reinterpret_cast<void*>(buffer), &first_byte, 1);

    int64_t n = sys_write(fd, buffer, 2 * PAGE, 0, 0, 0);
    if (n > 0) {
        worker.moved.fetch_add_relaxed(static_cast<uint64_t>(n));
    }

    (void)mm::mm_context_unmap(worker.job.ctx, buffer, 2 * PAGE);
    return n == static_cast<int64_t>(2 * PAGE);
}

static void run_file_worker(void* arg) {
    file_worker& worker = *static_cast<file_worker*>(arg);
    sched::task* self = sched::current();
    resource::handle_t h = -1;
    if (resource::open(self, worker.job.path, worker.job.open_flags, &h) != resource::OK) {
        worker.failures.fetch_add_relaxed(1);
        worker.done.store_release(1);
        sched::exit(0);
    }

    {
        user_space_scope scope(worker.job.ctx);
        uint32_t round = 0;
        while (worker.job.rounds ? round < worker.job.rounds : !worker.should_stop.load_acquire()) {
            if (!worker.job.call(worker, static_cast<uint64_t>(h), round)) {
                worker.failures.fetch_add_relaxed(1);
            }

            round++;
        }
    }

    (void)resource::close(self, h);
    worker.done.store_release(1);
    sched::exit(0);
}

// Starts `worker` on the CPU `cpu_offset` places after the caller's. The calling test runs on its CPU's idle task,
// which must never block, so only workers contend for a file's locks.
static sched::task* start_file_worker(file_worker& worker, const file_worker_job& job, uint32_t cpu_offset) {
    worker.job = job;
    worker.should_stop.store_relaxed(0);
    worker.moved.store_relaxed(0);
    worker.failures.store_relaxed(0);
    worker.done.store_relaxed(0);

    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(run_file_worker, &worker, "file_io_worker", sched::TASK_FLAG_ELEVATED);
        if (t) {
            t->add_ref();
            sched::enqueue_on(t, (percpu::current_cpu_id() + cpu_offset) % smp::cpu_count());
        }
    });

    return t;
}

static bool finish_file_worker(sched::task* t, file_worker& worker) {
    bool finished = test_helpers::finish_or_kill(t, worker.done);
    test_helpers::unpin(t);

    return finished;
}

TEST(file_io, pread_and_pwrite_cross_page_boundaries) {
    sched::task* task = sched::current();
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    uintptr_t source = map_user(ctx, 4, EAGER_ANON);
    uintptr_t target = map_user(ctx, 4, EAGER_ANON);
    ASSERT_NE(source, static_cast<uintptr_t>(0));
    ASSERT_NE(target, static_cast<uintptr_t>(0));
    fill_user_with_pattern(ctx, source, 4 * PAGE);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/file_io_boundaries", CREATE_RW, &h), resource::OK);

    const size_t lengths[] = {1, PAGE - 1, PAGE + 1, 3 * PAGE + 5};
    const uint64_t offsets[] = {0, 7, PAGE - 3, 2 * PAGE + 11};
    {
        user_space_scope scope(ctx);
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
            fill_user(ctx, target, 4 * PAGE, 0);
            int64_t length = static_cast<int64_t>(lengths[i]);
            EXPECT_EQ(sys_pwrite64(static_cast<uint64_t>(h), source, lengths[i], offsets[i], 0, 0), length);
            EXPECT_EQ(sys_pread64(static_cast<uint64_t>(h), target, lengths[i], offsets[i], 0, 0), length);
            EXPECT_TRUE(user_holds_pattern(ctx, target, lengths[i], 0));
        }

        uint64_t end = 2 * PAGE + 11 + 3 * PAGE + 5;
        EXPECT_EQ(sys_pread64(static_cast<uint64_t>(h), target, 64, end - 10, 0, 0), static_cast<int64_t>(10));
        EXPECT_EQ(sys_pread64(static_cast<uint64_t>(h), target, 64, end + 100, 0, 0), static_cast<int64_t>(0));
    }

    remove_file(h, "/file_io_boundaries");
    mm::mm_context_release(ctx);
}

TEST(file_io, read_into_a_lazy_buffer_faults_each_page_in) {
    resource::handle_t h = create_pattern_file("/file_io_lazy_read", 4 * PAGE);
    ASSERT_NE(h, -1);
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t lazy = map_user(ctx, 4, LAZY_ANON);
    ASSERT_NE(lazy, static_cast<uintptr_t>(0));

    {
        user_space_scope scope(ctx);
        EXPECT_EQ(sys_pread64(static_cast<uint64_t>(h), lazy, 4 * PAGE, 0, 0, 0), static_cast<int64_t>(4 * PAGE));
    }

    EXPECT_TRUE(user_holds_pattern(ctx, lazy, 4 * PAGE, 0));

    remove_file(h, "/file_io_lazy_read");
    mm::mm_context_release(ctx);
}

TEST(file_io, read_stops_at_a_missing_destination_page) {
    resource::handle_t h = create_pattern_file("/file_io_read_edge", 3 * PAGE);
    ASSERT_NE(h, -1);
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t buf = map_user(ctx, 1, EAGER_ANON);
    ASSERT_NE(buf, static_cast<uintptr_t>(0));

    uint64_t fd = static_cast<uint64_t>(h);
    {
        user_space_scope scope(ctx);
        EXPECT_EQ(sys_read(fd, buf, 2 * PAGE, 0, 0, 0), static_cast<int64_t>(PAGE));
        EXPECT_EQ(sys_lseek(fd, 0, fs::SEEK_CUR, 0, 0, 0), static_cast<int64_t>(PAGE));
        EXPECT_EQ(sys_read(fd, buf + PAGE, 16, 0, 0, 0), syscall::EFAULT);
        EXPECT_EQ(sys_lseek(fd, 0, fs::SEEK_CUR, 0, 0, 0), static_cast<int64_t>(PAGE));
    }

    EXPECT_TRUE(user_holds_pattern(ctx, buf, PAGE, 0));

    remove_file(h, "/file_io_read_edge");
    mm::mm_context_release(ctx);
}

TEST(file_io, write_keeps_only_the_bytes_before_a_missing_source_page) {
    sched::task* task = sched::current();
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t buf = map_user(ctx, 1, EAGER_ANON);
    ASSERT_NE(buf, static_cast<uintptr_t>(0));
    fill_user(ctx, buf, PAGE, 'w');

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/file_io_write_edge", CREATE_RW, &h), resource::OK);

    int64_t partial = 0;
    int64_t refused = 0;
    {
        user_space_scope scope(ctx);
        partial = sys_pwrite64(static_cast<uint64_t>(h), buf, 2 * PAGE, 100, 0, 0);
        refused = sys_pwrite64(static_cast<uint64_t>(h), buf + PAGE, 16, 8 * PAGE, 0, 0);
    }

    EXPECT_EQ(partial, static_cast<int64_t>(PAGE));
    EXPECT_EQ(refused, syscall::EFAULT);
    EXPECT_EQ(file_size(h), static_cast<int64_t>(100 + PAGE));

    uint8_t tail[16] = {};
    EXPECT_EQ(resource::read_at(task, h, tail, sizeof(tail), 100 + PAGE - 4), static_cast<ssize_t>(4));
    EXPECT_EQ(tail[0], 'w');

    remove_file(h, "/file_io_write_edge");
    mm::mm_context_release(ctx);
}

TEST(file_io, read_and_write_through_a_mapping_of_the_same_file) {
    sched::task* task = sched::current();
    resource::handle_t h = create_pattern_file("/file_io_self_map", 3 * PAGE);
    ASSERT_NE(h, -1);
    fs::file* f = fs::open("/file_io_self_map", fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    uintptr_t writable = 0;
    uintptr_t read_only = 0;
    ASSERT_EQ(fs::mmap(f, ctx, 0, 3 * PAGE, PROT_RW, mm::MM_PROT_MASK, mm::MM_MAP_SHARED, 0, &writable),
              mm::MM_CTX_OK);
    ASSERT_EQ(fs::mmap(f, ctx, 0, PAGE, mm::MM_PROT_READ, mm::MM_PROT_MASK, mm::MM_MAP_SHARED, 0, &read_only),
              mm::MM_CTX_OK);

    uint64_t fd = static_cast<uint64_t>(h);
    {
        user_space_scope scope(ctx);
        EXPECT_EQ(sys_pread64(fd, writable + 2 * PAGE, 100, 0, 0, 0), static_cast<int64_t>(100));
        EXPECT_EQ(sys_pwrite64(fd, writable, 100, PAGE + 50, 0, 0), static_cast<int64_t>(100));
        EXPECT_EQ(sys_pread64(fd, read_only, 16, 0, 0, 0), syscall::EFAULT);
    }

    uint8_t copied[100];
    EXPECT_EQ(resource::read_at(task, h, copied, sizeof(copied), 2 * PAGE), static_cast<ssize_t>(100));
    EXPECT_EQ(copied[99], pattern_byte(99));
    EXPECT_EQ(resource::read_at(task, h, copied, sizeof(copied), PAGE + 50), static_cast<ssize_t>(100));
    EXPECT_EQ(copied[99], pattern_byte(99));

    mm::mm_context_release(ctx);
    (void)fs::close(f);
    remove_file(h, "/file_io_self_map");
}

TEST(file_io, mapping_the_file_while_reading_it_never_deadlocks) {
    resource::handle_t h = create_pattern_file("/file_io_remap", 2 * PAGE);
    ASSERT_NE(h, -1);
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    file_worker_job reads = {
        .call = read_into_a_lazy_buffer, .path = "/file_io_remap", .open_flags = fs::O_RDONLY,
        .ctx = ctx, .bytes = 0, .size = 2 * PAGE, .rounds = LAZY_READ_ROUNDS,
    };

    file_worker_job maps = {
        .call = map_and_unmap_the_file, .path = "/file_io_remap", .open_flags = fs::O_RDONLY,
        .ctx = ctx, .bytes = 0, .size = PAGE, .rounds = REMAP_ROUNDS,
    };

    sched::task* reader = start_file_worker(g_first_worker, reads, 1);
    ASSERT_NOT_NULL(reader);
    sched::task* mapper = start_file_worker(g_second_worker, maps, 2);
    ASSERT_NOT_NULL(mapper);

    EXPECT_TRUE(finish_file_worker(reader, g_first_worker));
    EXPECT_TRUE(finish_file_worker(mapper, g_second_worker));
    EXPECT_EQ(g_first_worker.failures.load_acquire(), 0u);
    EXPECT_EQ(g_second_worker.failures.load_acquire(), 0u);

    remove_file(h, "/file_io_remap");
    mm::mm_context_release(ctx);
}

TEST(file_io, writes_are_atomic_against_reads) {
    sched::task* task = sched::current();
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    uintptr_t records = map_user(ctx, 2 * RECORD_BYTES / PAGE, EAGER_ANON);
    uintptr_t target = map_user(ctx, RECORD_BYTES / PAGE, EAGER_ANON);
    ASSERT_NE(records, static_cast<uintptr_t>(0));
    ASSERT_NE(target, static_cast<uintptr_t>(0));

    fill_user(ctx, records, RECORD_BYTES, 'A');
    fill_user(ctx, records + RECORD_BYTES, RECORD_BYTES, 'B');

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/file_io_records", CREATE_RW, &h), resource::OK);
    {
        user_space_scope scope(ctx);
        ASSERT_EQ(sys_pwrite64(static_cast<uint64_t>(h), records, RECORD_BYTES, 0, 0, 0),
                  static_cast<int64_t>(RECORD_BYTES));
    }

    file_worker_job writes = {
        .call = write_records_in_turn, .path = "/file_io_records", .open_flags = fs::O_WRONLY,
        .ctx = ctx, .bytes = records, .size = RECORD_BYTES, .rounds = RECORD_ROUNDS,
    };

    file_worker_job reads = {
        .call = read_a_whole_record, .path = "/file_io_records", .open_flags = fs::O_RDONLY,
        .ctx = ctx, .bytes = target, .size = RECORD_BYTES, .rounds = RECORD_ROUNDS,
    };

    sched::task* writer = start_file_worker(g_first_worker, writes, 1);
    ASSERT_NOT_NULL(writer);
    sched::task* reader = start_file_worker(g_second_worker, reads, 2);
    ASSERT_NOT_NULL(reader);

    EXPECT_TRUE(finish_file_worker(writer, g_first_worker));
    EXPECT_TRUE(finish_file_worker(reader, g_second_worker));
    EXPECT_EQ(g_first_worker.failures.load_acquire(), 0u);
    EXPECT_EQ(g_second_worker.failures.load_acquire(), 0u);

    remove_file(h, "/file_io_records");
    mm::mm_context_release(ctx);
}

TEST(file_io, appends_never_interleave) {
    sched::task* task = sched::current();
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    uintptr_t first_record = map_user(ctx, 3, EAGER_ANON);
    uintptr_t second_record = map_user(ctx, 3, EAGER_ANON);
    ASSERT_NE(first_record, static_cast<uintptr_t>(0));
    ASSERT_NE(second_record, static_cast<uintptr_t>(0));

    fill_user(ctx, first_record, APPEND_RECORD_BYTES, 'A');
    fill_user(ctx, second_record, APPEND_RECORD_BYTES, 'B');

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/file_io_appends", CREATE_RW, &h), resource::OK);

    file_worker_job first_appends = {
        .call = append_the_bytes, .path = "/file_io_appends", .open_flags = OPEN_APPEND,
        .ctx = ctx, .bytes = first_record, .size = APPEND_RECORD_BYTES, .rounds = APPEND_ROUNDS,
    };

    file_worker_job second_appends = first_appends;
    second_appends.bytes = second_record;
    sched::task* first = start_file_worker(g_first_worker, first_appends, 1);
    ASSERT_NOT_NULL(first);
    sched::task* second = start_file_worker(g_second_worker, second_appends, 2);
    ASSERT_NOT_NULL(second);

    EXPECT_TRUE(finish_file_worker(first, g_first_worker));
    EXPECT_TRUE(finish_file_worker(second, g_second_worker));
    ASSERT_EQ(file_size(h), static_cast<int64_t>(2 * APPEND_ROUNDS * APPEND_RECORD_BYTES));

    uint32_t mixed_records = 0;
    for (uint32_t record = 0; record < 2 * APPEND_ROUNDS; record++) {
        if (!file_holds_one_value(h, record * APPEND_RECORD_BYTES, APPEND_RECORD_BYTES)) {
            mixed_records++;
        }
    }

    EXPECT_EQ(mixed_records, 0u);

    remove_file(h, "/file_io_appends");
    mm::mm_context_release(ctx);
}

TEST(file_io, a_split_append_never_overwrites_another) {
    sched::task* task = sched::current();
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t one_byte = map_user(ctx, 1, EAGER_ANON);
    ASSERT_NE(one_byte, static_cast<uintptr_t>(0));
    fill_user(ctx, one_byte, 1, 'b');

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/file_io_split_appends", CREATE_RW, &h), resource::OK);

    file_worker_job split_appends = {
        .call = append_a_half_lazy_buffer, .path = "/file_io_split_appends", .open_flags = OPEN_APPEND,
        .ctx = ctx, .bytes = 0, .size = 2 * PAGE, .rounds = SPLIT_APPEND_ROUNDS,
    };

    file_worker_job byte_appends = {
        .call = append_the_bytes, .path = "/file_io_split_appends", .open_flags = OPEN_APPEND,
        .ctx = ctx, .bytes = one_byte, .size = 1, .rounds = 0,
    };

    sched::task* splitter = start_file_worker(g_first_worker, split_appends, 1);
    ASSERT_NOT_NULL(splitter);
    sched::task* byte_appender = start_file_worker(g_second_worker, byte_appends, 2);
    ASSERT_NOT_NULL(byte_appender);

    EXPECT_TRUE(finish_file_worker(splitter, g_first_worker));
    g_second_worker.should_stop.store_release(1);
    EXPECT_TRUE(finish_file_worker(byte_appender, g_second_worker));

    uint64_t appended = g_first_worker.moved.load_acquire() + g_second_worker.moved.load_acquire();
    EXPECT_EQ(g_first_worker.failures.load_acquire(), 0u);
    EXPECT_EQ(g_second_worker.failures.load_acquire(), 0u);
    EXPECT_EQ(file_size(h), static_cast<int64_t>(appended));

    remove_file(h, "/file_io_split_appends");
    mm::mm_context_release(ctx);
}

TEST(file_io, readv_and_writev_carry_stdio_shapes_through_files_and_pipes) {
    sched::task* task = sched::current();
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    uintptr_t lists = map_user(ctx, 1, EAGER_ANON);
    uintptr_t source = map_user(ctx, 1, EAGER_ANON);
    uintptr_t target = map_user(ctx, 2, EAGER_ANON);
    ASSERT_NE(lists, static_cast<uintptr_t>(0));
    ASSERT_NE(source, static_cast<uintptr_t>(0));
    ASSERT_NE(target, static_cast<uintptr_t>(0));

    fill_user_with_pattern(ctx, source, PAGE);
    auto* list = reinterpret_cast<syscall::iovec*>(user_byte(ctx, lists));
    uintptr_t pipe_handles = lists + PAGE / 2;

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/file_io_vectors", CREATE_RW, &h), resource::OK);
    uint64_t fd = static_cast<uint64_t>(h);

    // musl's __stdio_write passes the FILE's buffered bytes, then the caller's, and __stdio_read the caller's
    // buffer, then the FILE's 1 KiB buffer
    list[0] = {source, 1000};
    list[1] = {source + 1000, 3000};
    {
        user_space_scope scope(ctx);
        EXPECT_EQ(sys_writev(fd, lists, 2, 0, 0, 0), static_cast<int64_t>(4000));
        EXPECT_EQ(sys_lseek(fd, 0, fs::SEEK_SET, 0, 0, 0), static_cast<int64_t>(0));
    }

    list[0] = {target, 3095};
    list[1] = {target + PAGE, 1024};
    {
        user_space_scope scope(ctx);
        EXPECT_EQ(sys_readv(fd, lists, 2, 0, 0, 0), static_cast<int64_t>(4000));
    }

    EXPECT_TRUE(user_holds_pattern(ctx, target, 3095, 0));
    EXPECT_TRUE(user_holds_pattern(ctx, target + PAGE, 905, 3095));

    {
        user_space_scope scope(ctx);
        ASSERT_EQ(sys_pipe2(pipe_handles, 0, 0, 0, 0, 0), static_cast<int64_t>(0));
    }

    auto* pipe_ends = reinterpret_cast<int32_t*>(user_byte(ctx, pipe_handles));
    uint64_t read_end = static_cast<uint64_t>(pipe_ends[0]);
    uint64_t write_end = static_cast<uint64_t>(pipe_ends[1]);
    list[0] = {source, 100};
    list[1] = {source + 100, 200};
    {
        user_space_scope scope(ctx);
        EXPECT_EQ(sys_writev(write_end, lists, 2, 0, 0, 0), static_cast<int64_t>(300));
    }

    list[0] = {target, 200};
    list[1] = {target + PAGE, 1024};
    {
        user_space_scope scope(ctx);
        EXPECT_EQ(sys_readv(read_end, lists, 2, 0, 0, 0), static_cast<int64_t>(300));
    }

    EXPECT_TRUE(user_holds_pattern(ctx, target, 200, 0));
    EXPECT_TRUE(user_holds_pattern(ctx, target + PAGE, 100, 200));

    (void)resource::close(task, static_cast<resource::handle_t>(read_end));
    (void)resource::close(task, static_cast<resource::handle_t>(write_end));
    remove_file(h, "/file_io_vectors");
    mm::mm_context_release(ctx);
}
