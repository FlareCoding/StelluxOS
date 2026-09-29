#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "exec/elf.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "fs/procfs/procfs.h"
#include "mm/heap.h"
#include "resource/providers/proc_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"

TEST_SUITE(procfs);

static constexpr const char* PROGRAM_DIR = "/bin";
static constexpr const char* PROGRAM_NAME = "hello";
static constexpr const char* PROGRAM_PATH = "/bin/hello";
static constexpr const char* UNLINKED_PATH = "/procfs_unlinked_exe";
static constexpr const char* UNLINKED_EXE = "/procfs_unlinked_exe (deleted)";
static constexpr size_t SHORT_BUFFER = 4;

static void release_node(fs::node* n) {
    if (n && n->release()) {
        fs::node::ref_destroy(n);
    }
}

static bool copy_file(const char* from, const char* to) {
    fs::file* src = fs::open(from, fs::O_RDONLY);
    if (!src) {
        return false;
    }

    fs::vattr attr;
    uint8_t* bytes = nullptr;
    bool read_whole = false;
    if (fs::fstat(src, &attr) == fs::OK) {
        bytes = static_cast<uint8_t*>(heap::ualloc(attr.size));
        read_whole = bytes && fs::read(src, bytes, attr.size) == static_cast<ssize_t>(attr.size);
    }

    fs::close(src);

    fs::file* dst = read_whole ? fs::open(to, fs::O_CREAT | fs::O_RDWR | fs::O_TRUNC) : nullptr;
    bool copied = dst && fs::write(dst, bytes, attr.size) == static_cast<ssize_t>(attr.size);
    if (dst) {
        fs::close(dst);
    }

    if (bytes) {
        heap::ufree(bytes);
    }

    return copied;
}

TEST(procfs, a_process_names_its_program_by_absolute_path) {
    fs::node* dir = nullptr;
    ASSERT_EQ(fs::lookup(PROGRAM_DIR, &dir), fs::OK);

    exec::loaded_image loaded;
    int32_t rc = exec::load_elf(PROGRAM_NAME, &loaded, dir);
    release_node(dir);
    ASSERT_EQ(rc, exec::OK);
    ASSERT_NOT_NULL(loaded.program);

    sched::task* child = sched::create_user_task(&loaded, PROGRAM_NAME);
    ASSERT_NOT_NULL(child);
    EXPECT_NULL(loaded.program);

    char path[64];
    size_t len = 0;
    EXPECT_EQ(procfs::exe_path(child, path, sizeof(path), &len), fs::OK);
    EXPECT_EQ(len, string::strlen(PROGRAM_PATH));
    EXPECT_EQ(string::memcmp(path, PROGRAM_PATH, len), 0);

    EXPECT_EQ(procfs::exe_path(child, path, SHORT_BUFFER, &len), fs::OK);
    EXPECT_EQ(len, SHORT_BUFFER);
    EXPECT_EQ(string::memcmp(path, PROGRAM_PATH, SHORT_BUFFER), 0);

    resource::proc_provider::destroy_unstarted_task(child);
}

TEST(procfs, an_unlinked_program_names_the_path_it_started_from) {
    ASSERT_TRUE(copy_file(PROGRAM_PATH, UNLINKED_PATH));

    exec::loaded_image loaded;
    ASSERT_EQ(exec::load_elf(UNLINKED_PATH, &loaded), exec::OK);
    sched::task* child = sched::create_user_task(&loaded, UNLINKED_PATH);
    ASSERT_NOT_NULL(child);

    EXPECT_EQ(fs::unlink(UNLINKED_PATH), fs::OK);

    char path[64];
    size_t len = 0;
    EXPECT_EQ(procfs::exe_path(child, path, sizeof(path), &len), fs::OK);
    EXPECT_EQ(len, string::strlen(UNLINKED_EXE));
    EXPECT_EQ(string::memcmp(path, UNLINKED_EXE, len), 0);

    resource::proc_provider::destroy_unstarted_task(child);
}

TEST(procfs, self_exe_is_a_link_that_needs_a_program) {
    fs::node* link = nullptr;
    ASSERT_EQ(fs::lookup_at(nullptr, "/proc/self/exe", fs::LOOKUP_NOFOLLOW, &link), fs::OK);
    EXPECT_EQ(link->type(), fs::node_type::symlink);

    char buf[16];
    size_t len = 0;
    EXPECT_EQ(link->readlink(buf, sizeof(buf), &len), fs::ERR_NOENT);

    release_node(link);
}
