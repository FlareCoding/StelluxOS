#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_io.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "fs/fstypes.h"
#include "common/string.h"
#include "sched/sched.h"
#include "sched/task.h"

TEST_SUITE(vectored_io);

using test_helpers::user_page;
using test_helpers::user_space_scope;

// The buffer list sits at the start of the page, and the buffers it points at follow it here
constexpr size_t BUFFER_AREA_OFFSET = 1024;
constexpr uint64_t LONGEST_REPORTABLE_LENGTH = 0x7FFFFFFFFFFFFFFF;

constexpr int64_t AT_FILE_POSITION = -1;
constexpr uint64_t RWF_HIPRI = 0x01;
constexpr uint64_t RWF_DSYNC = 0x02;
constexpr uint64_t RWF_SYNC = 0x04;
constexpr uint64_t RWF_NOWAIT = 0x08;
constexpr uint64_t RWF_APPEND = 0x10;
constexpr uint64_t UNKNOWN_RWF_FLAG = 0x100;

static char* lay_out_buffers(user_page& page, const uint64_t* lengths, size_t count) {
    auto* iovs = page.at<syscall::iovec>(0);
    uint64_t next_buffer = page.addr + BUFFER_AREA_OFFSET;
    for (size_t i = 0; i < count; i++) {
        iovs[i] = {next_buffer, lengths[i]};
        next_buffer += lengths[i];
    }

    return page.at<char>(BUFFER_AREA_OFFSET);
}

static int64_t call_preadv(user_page& page, resource::handle_t h, uint64_t iovcnt, int64_t offset) {
    user_space_scope scope(page.ctx);
    return sys_preadv(static_cast<uint64_t>(h), page.addr, iovcnt, static_cast<uint64_t>(offset), 0, 0);
}

static int64_t call_pwritev(user_page& page, resource::handle_t h, uint64_t iovcnt, int64_t offset) {
    user_space_scope scope(page.ctx);
    return sys_pwritev(static_cast<uint64_t>(h), page.addr, iovcnt, static_cast<uint64_t>(offset), 0, 0);
}

static int64_t call_preadv2(user_page& page, resource::handle_t h, uint64_t iovcnt, int64_t offset, uint64_t flags) {
    user_space_scope scope(page.ctx);
    return sys_preadv2(static_cast<uint64_t>(h), page.addr, iovcnt, static_cast<uint64_t>(offset), 0, flags);
}

static int64_t call_pwritev2(user_page& page, resource::handle_t h, uint64_t iovcnt, int64_t offset, uint64_t flags) {
    user_space_scope scope(page.ctx);
    return sys_pwritev2(static_cast<uint64_t>(h), page.addr, iovcnt, static_cast<uint64_t>(offset), 0, flags);
}

static int64_t create_pipe_into(user_page& page) {
    user_space_scope scope(page.ctx);
    return sys_pipe2(page.addr, 0, 0, 0, 0, 0);
}

TEST(vectored_io, preadv_fills_each_buffer_from_where_the_last_one_ended) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    uint32_t flags = fs::O_CREAT | fs::O_TRUNC | fs::O_RDWR;
    ASSERT_EQ(resource::open(task, "/vectored_read", flags, &h), resource::OK);
    ASSERT_EQ(resource::write_at(task, h, "0123456789", 10, 0), static_cast<ssize_t>(10));

    const uint64_t lengths[] = {3, 0, 4};
    char* buffers = lay_out_buffers(page, lengths, 3);
    ASSERT_EQ(call_preadv(page, h, 3, 2), static_cast<int64_t>(7));
    EXPECT_EQ(string::memcmp(buffers, "2345678", 7), 0);

    // The file position never moved, so a plain read starts at the beginning
    char first_bytes[4] = {};
    ASSERT_EQ(resource::read(task, h, first_bytes, sizeof(first_bytes)), static_cast<ssize_t>(4));
    EXPECT_EQ(string::memcmp(first_bytes, "0123", 4), 0);

    resource::close(task, h);
}

TEST(vectored_io, pwritev_writes_the_buffers_back_to_back) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    uint32_t flags = fs::O_CREAT | fs::O_TRUNC | fs::O_RDWR;
    ASSERT_EQ(resource::open(task, "/vectored_write", flags, &h), resource::OK);

    const uint64_t lengths[] = {2, 3};
    char* buffers = lay_out_buffers(page, lengths, 2);
    string::memcpy(buffers, "abcde", 5);
    ASSERT_EQ(call_pwritev(page, h, 2, 4), static_cast<int64_t>(5));

    char contents[9] = {};
    ASSERT_EQ(resource::read(task, h, contents, sizeof(contents)), static_cast<ssize_t>(9));
    EXPECT_EQ(string::memcmp(contents, "\0\0\0\0abcde", 9), 0);

    resource::close(task, h);
}

TEST(vectored_io, a_short_read_ends_the_call) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    uint32_t flags = fs::O_CREAT | fs::O_TRUNC | fs::O_RDWR;
    ASSERT_EQ(resource::open(task, "/vectored_short", flags, &h), resource::OK);
    ASSERT_EQ(resource::write_at(task, h, "abcde", 5, 0), static_cast<ssize_t>(5));

    const uint64_t lengths[] = {4, 4};
    char* buffers = lay_out_buffers(page, lengths, 2);
    EXPECT_EQ(call_preadv(page, h, 2, 0), static_cast<int64_t>(5));
    EXPECT_EQ(string::memcmp(buffers, "abcde", 5), 0);

    resource::close(task, h);
}

TEST(vectored_io, malformed_calls_and_unseekable_handles_are_refused) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/vectored_malformed", fs::O_CREAT | fs::O_RDWR, &h), resource::OK);

    const uint64_t overflowing_lengths[] = {LONGEST_REPORTABLE_LENGTH, 1};
    lay_out_buffers(page, overflowing_lengths, 2);
    EXPECT_EQ(call_preadv(page, h, 2, 0), syscall::EINVAL);

    const uint64_t one_byte[] = {1};
    lay_out_buffers(page, one_byte, 1);
    EXPECT_EQ(call_preadv(page, h, 1, -1), syscall::EINVAL);
    EXPECT_EQ(call_preadv(page, h, 1, fs::MAX_FILE_OFFSET), syscall::EINVAL);
    EXPECT_EQ(call_preadv(page, h, syscall::MAX_IOVCNT + 1, 0), syscall::EINVAL);
    EXPECT_EQ(call_preadv(page, h, 0, 0), static_cast<int64_t>(0));

    ASSERT_EQ(create_pipe_into(page), static_cast<int64_t>(0));
    int32_t pipe_read_end = page.at<int32_t>(0)[0];
    int32_t pipe_write_end = page.at<int32_t>(0)[1];
    lay_out_buffers(page, one_byte, 1);
    EXPECT_EQ(call_preadv(page, pipe_read_end, 1, 0), syscall::ESPIPE);

    resource::close(task, pipe_read_end);
    resource::close(task, pipe_write_end);
    resource::close(task, h);
}

TEST(vectored_io, the_v2_calls_use_and_advance_the_file_position_at_offset_minus_one) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    uint32_t flags = fs::O_CREAT | fs::O_TRUNC | fs::O_RDWR;
    ASSERT_EQ(resource::open(task, "/vectored_position", flags, &h), resource::OK);
    ASSERT_EQ(resource::write_at(task, h, "0123456789", 10, 0), static_cast<ssize_t>(10));

    const uint64_t two_pairs[] = {2, 2};
    char* buffers = lay_out_buffers(page, two_pairs, 2);
    ASSERT_EQ(call_preadv2(page, h, 2, AT_FILE_POSITION, 0), static_cast<int64_t>(4));
    EXPECT_EQ(string::memcmp(buffers, "0123", 4), 0);

    const uint64_t three_bytes[] = {3};
    buffers = lay_out_buffers(page, three_bytes, 1);
    ASSERT_EQ(call_preadv2(page, h, 1, AT_FILE_POSITION, 0), static_cast<int64_t>(3));
    EXPECT_EQ(string::memcmp(buffers, "456", 3), 0);

    const uint64_t two_bytes[] = {2};
    buffers = lay_out_buffers(page, two_bytes, 1);
    string::memcpy(buffers, "xy", 2);
    ASSERT_EQ(call_pwritev2(page, h, 1, AT_FILE_POSITION, 0), static_cast<int64_t>(2));

    char contents[10] = {};
    ASSERT_EQ(resource::read_at(task, h, contents, sizeof(contents), 0), static_cast<ssize_t>(10));
    EXPECT_EQ(string::memcmp(contents, "0123456xy9", 10), 0);

    resource::close(task, h);
}

TEST(vectored_io, the_v2_calls_accept_hints_but_refuse_append_and_unknown_flags) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    uint32_t flags = fs::O_CREAT | fs::O_TRUNC | fs::O_RDWR;
    ASSERT_EQ(resource::open(task, "/vectored_flags", flags, &h), resource::OK);
    ASSERT_EQ(resource::write_at(task, h, "abc", 3, 0), static_cast<ssize_t>(3));

    const uint64_t one_byte[] = {1};
    lay_out_buffers(page, one_byte, 1);
    uint64_t hints = RWF_HIPRI | RWF_DSYNC | RWF_SYNC | RWF_NOWAIT;
    EXPECT_EQ(call_preadv2(page, h, 1, 0, hints), static_cast<int64_t>(1));
    EXPECT_EQ(call_pwritev2(page, h, 1, 0, hints), static_cast<int64_t>(1));

    EXPECT_EQ(call_pwritev2(page, h, 1, 0, RWF_APPEND), syscall::EOPNOTSUPP);
    EXPECT_EQ(call_preadv2(page, h, 1, 0, UNKNOWN_RWF_FLAG), syscall::EOPNOTSUPP);
    EXPECT_EQ(call_preadv2(page, h, 1, AT_FILE_POSITION - 1, 0), syscall::EINVAL);

    resource::close(task, h);
}

TEST(vectored_io, nowait_returns_instead_of_waiting_on_an_empty_pipe) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(create_pipe_into(page), static_cast<int64_t>(0));
    int32_t pipe_read_end = page.at<int32_t>(0)[0];
    int32_t pipe_write_end = page.at<int32_t>(0)[1];

    const uint64_t one_byte[] = {1};
    char* buffers = lay_out_buffers(page, one_byte, 1);
    EXPECT_EQ(call_preadv2(page, pipe_read_end, 1, AT_FILE_POSITION, RWF_NOWAIT), syscall::EAGAIN);

    ASSERT_EQ(resource::write(task, pipe_write_end, "z", 1), static_cast<ssize_t>(1));
    EXPECT_EQ(call_preadv2(page, pipe_read_end, 1, AT_FILE_POSITION, RWF_NOWAIT), static_cast<int64_t>(1));
    EXPECT_EQ(buffers[0], 'z');

    resource::close(task, pipe_read_end);
    resource::close(task, pipe_write_end);
}
