#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/node.h"
#include "common/string.h"
#include "common/logging.h"

TEST_SUITE(fs_test);

static void release_node(fs::node* n) {
    if (!n) {
        return;
    }

    if (n->release()) {
        fs::node::ref_destroy(n);
    }
}

TEST(fs_test, create_and_close_file) {
    fs::file* f = fs::open("/test_create", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(fs::close(f), fs::OK);
    fs::unlink("/test_create");
}

TEST(fs_test, open_nonexistent_returns_null) {
    fs::file* f = fs::open("/does_not_exist", fs::O_RDONLY);
    EXPECT_NULL(f);
}

TEST(fs_test, write_and_read_back) {
    fs::file* f = fs::open("/test_rw", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    const char* msg = "hello stellux";
    ssize_t written = fs::write(f, msg, 13);
    EXPECT_EQ(written, static_cast<ssize_t>(13));

    fs::seek(f, 0, fs::SEEK_SET);

    char buf[32] = {};
    ssize_t rd = fs::read(f, buf, sizeof(buf));
    EXPECT_EQ(rd, static_cast<ssize_t>(13));
    EXPECT_STREQ(buf, "hello stellux");

    fs::close(f);
    fs::unlink("/test_rw");
}

TEST(fs_test, write_append_mode) {
    fs::file* f = fs::open("/test_append", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::write(f, "AAA", 3);
    fs::close(f);

    f = fs::open("/test_append", fs::O_WRONLY | fs::O_APPEND);
    ASSERT_NOT_NULL(f);
    fs::write(f, "BBB", 3);
    fs::close(f);

    f = fs::open("/test_append", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);
    char buf[16] = {};
    ssize_t rd = fs::read(f, buf, sizeof(buf));
    EXPECT_EQ(rd, static_cast<ssize_t>(6));
    EXPECT_STREQ(buf, "AAABBB");

    fs::close(f);
    fs::unlink("/test_append");
}

TEST(fs_test, seek_set_cur_end) {
    fs::file* f = fs::open("/test_seek", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::write(f, "0123456789", 10);

    EXPECT_EQ(fs::seek(f, 0, fs::SEEK_SET), static_cast<int64_t>(0));
    EXPECT_EQ(fs::seek(f, 0, fs::SEEK_END), static_cast<int64_t>(10));
    EXPECT_EQ(fs::seek(f, -5, fs::SEEK_CUR), static_cast<int64_t>(5));

    char buf[4] = {};
    ssize_t rd = fs::read(f, buf, 3);
    EXPECT_EQ(rd, static_cast<ssize_t>(3));
    EXPECT_STREQ(buf, "567");

    fs::close(f);
    fs::unlink("/test_seek");
}

TEST(fs_test, mkdir_and_stat) {
    int32_t err = fs::mkdir("/test_dir", 0);
    EXPECT_EQ(err, fs::OK);

    fs::vattr attr;
    err = fs::stat("/test_dir", &attr);
    EXPECT_EQ(err, fs::OK);
    EXPECT_EQ(static_cast<uint32_t>(attr.type),
              static_cast<uint32_t>(fs::node_type::directory));

    fs::rmdir("/test_dir");
}

TEST(fs_test, mkdir_duplicate_fails) {
    EXPECT_EQ(fs::mkdir("/dup_dir", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/dup_dir", 0), fs::ERR_EXIST);
    fs::rmdir("/dup_dir");
}

TEST(fs_test, rmdir_nonempty_fails) {
    fs::mkdir("/parent", 0);
    fs::mkdir("/parent/child", 0);

    EXPECT_EQ(fs::rmdir("/parent"), fs::ERR_NOTEMPTY);

    fs::rmdir("/parent/child");
    EXPECT_EQ(fs::rmdir("/parent"), fs::OK);
}

TEST(fs_test, rmdir_refuses_mount_point) {
    EXPECT_EQ(fs::rmdir("/dev"), fs::ERR_BUSY);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat("/dev/null", &attr), fs::OK);
}

TEST(fs_test, readdir_lists_children) {
    fs::mkdir("/rd_test", 0);
    fs::file* f1 = fs::open("/rd_test/a", fs::O_CREAT | fs::O_RDWR);
    fs::close(f1);
    fs::file* f2 = fs::open("/rd_test/b", fs::O_CREAT | fs::O_RDWR);
    fs::close(f2);

    fs::file* dir = fs::open("/rd_test", fs::O_RDONLY);
    ASSERT_NOT_NULL(dir);

    fs::dirent entries[4];
    ssize_t n = fs::readdir(dir, entries, 4);
    EXPECT_EQ(n, static_cast<ssize_t>(2));

    fs::close(dir);
    fs::unlink("/rd_test/a");
    fs::unlink("/rd_test/b");
    fs::rmdir("/rd_test");
}

TEST(fs_test, path_root_resolves) {
    fs::vattr attr;
    int32_t err = fs::stat("/", &attr);
    EXPECT_EQ(err, fs::OK);
    EXPECT_EQ(static_cast<uint32_t>(attr.type),
              static_cast<uint32_t>(fs::node_type::directory));
}

TEST(fs_test, path_dotdot_at_root) {
    fs::vattr attr;
    int32_t err = fs::stat("/..", &attr);
    EXPECT_EQ(err, fs::OK);
    EXPECT_EQ(static_cast<uint32_t>(attr.type),
              static_cast<uint32_t>(fs::node_type::directory));
}

TEST(fs_test, path_trailing_slashes) {
    fs::mkdir("/trailing", 0);
    fs::vattr attr;
    EXPECT_EQ(fs::stat("/trailing/", &attr), fs::OK);
    EXPECT_EQ(fs::stat("/trailing///", &attr), fs::OK);
    fs::rmdir("/trailing");
}

TEST(fs_test, path_dot_skipped) {
    fs::mkdir("/dottest", 0);
    fs::vattr attr;
    EXPECT_EQ(fs::stat("/./dottest", &attr), fs::OK);
    EXPECT_EQ(fs::stat("/dottest/.", &attr), fs::OK);
    fs::rmdir("/dottest");
}

TEST(fs_test, nested_dir_dotdot) {
    fs::mkdir("/a", 0);
    fs::mkdir("/a/b", 0);

    fs::file* f = fs::open("/a/b/../b/../b/../../a/b/../test",
                            fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    fs::write(f, "OK", 2);
    fs::close(f);

    f = fs::open("/a/test", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);
    char buf[4] = {};
    fs::read(f, buf, 2);
    EXPECT_STREQ(buf, "OK");
    fs::close(f);

    fs::unlink("/a/test");
    fs::rmdir("/a/b");
    fs::rmdir("/a");
}

TEST(fs_test, lookup_at_resolves_relative_paths) {
    EXPECT_EQ(fs::mkdir("/lookup_at_base", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/lookup_at_base/sub", 0), fs::OK);

    fs::file* f = fs::open("/lookup_at_base/sub/file", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    fs::node* base = nullptr;
    ASSERT_EQ(fs::lookup("/lookup_at_base", &base), fs::OK);

    fs::node* resolved = nullptr;
    ASSERT_EQ(fs::lookup_at(base, "sub/file", &resolved), fs::OK);
    EXPECT_EQ(static_cast<uint32_t>(resolved->type()),
              static_cast<uint32_t>(fs::node_type::regular));

    release_node(resolved);
    release_node(base);

    fs::unlink("/lookup_at_base/sub/file");
    fs::rmdir("/lookup_at_base/sub");
    fs::rmdir("/lookup_at_base");
}

TEST(fs_test, lookup_at_absolute_path_ignores_base) {
    EXPECT_EQ(fs::mkdir("/lookup_at_abs_a", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/lookup_at_abs_b", 0), fs::OK);

    fs::node* base = nullptr;
    ASSERT_EQ(fs::lookup("/lookup_at_abs_a", &base), fs::OK);

    fs::node* resolved = nullptr;
    ASSERT_EQ(fs::lookup_at(base, "/lookup_at_abs_b", &resolved), fs::OK);
    EXPECT_EQ(static_cast<uint32_t>(resolved->type()),
              static_cast<uint32_t>(fs::node_type::directory));

    release_node(resolved);
    release_node(base);

    fs::rmdir("/lookup_at_abs_a");
    fs::rmdir("/lookup_at_abs_b");
}

TEST(fs_test, resolve_parent_path_at_resolves_parent_under_base) {
    EXPECT_EQ(fs::mkdir("/parent_at", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/parent_at/sub", 0), fs::OK);

    fs::node* base = nullptr;
    ASSERT_EQ(fs::lookup("/parent_at", &base), fs::OK);

    fs::node* parent = nullptr;
    const char* name = nullptr;
    size_t name_len = 0;
    ASSERT_EQ(
        fs::resolve_parent_path_at(base, "sub/child", &parent, &name, &name_len),
        fs::OK);
    ASSERT_NOT_NULL(parent);
    ASSERT_NOT_NULL(name);
    EXPECT_EQ(name_len, static_cast<size_t>(5));
    EXPECT_EQ(string::strncmp(name, "child", name_len), 0);

    char parent_path[fs::PATH_MAX];
    ASSERT_EQ(fs::path_from_node(parent, parent_path, sizeof(parent_path)), fs::OK);
    EXPECT_STREQ(parent_path, "/parent_at/sub");

    release_node(parent);
    release_node(base);

    fs::rmdir("/parent_at/sub");
    fs::rmdir("/parent_at");
}

TEST(fs_test, path_from_node_returns_absolute_path) {
    EXPECT_EQ(fs::mkdir("/path_from", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/path_from/node", 0), fs::OK);

    fs::node* n = nullptr;
    ASSERT_EQ(fs::lookup("/path_from/node", &n), fs::OK);

    char path_buf[fs::PATH_MAX];
    ASSERT_EQ(fs::path_from_node(n, path_buf, sizeof(path_buf)), fs::OK);
    EXPECT_STREQ(path_buf, "/path_from/node");

    release_node(n);
    fs::rmdir("/path_from/node");
    fs::rmdir("/path_from");
}

TEST(fs_test, path_from_node_unlinked_node_returns_noent) {
    fs::file* f = fs::open("/path_unlinked", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    fs::node* n = nullptr;
    ASSERT_EQ(fs::lookup("/path_unlinked", &n), fs::OK);

    ASSERT_EQ(fs::unlink("/path_unlinked"), fs::OK);

    char path_buf[fs::PATH_MAX];
    EXPECT_EQ(fs::path_from_node(n, path_buf, sizeof(path_buf)), fs::ERR_NOENT);

    release_node(n);
}

TEST(fs_test, stat_nonexistent) {
    fs::vattr attr;
    EXPECT_EQ(fs::stat("/nope", &attr), fs::ERR_NOENT);
}

TEST(fs_test, err_notdir) {
    fs::file* f = fs::open("/notdir_file", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    fs::vattr attr;
    EXPECT_EQ(fs::stat("/notdir_file/child", &attr), fs::ERR_NOTDIR);

    fs::unlink("/notdir_file");
}

TEST(fs_test, stat_identity_distinct_per_node) {
    fs::file* a = fs::open("/ino_a", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(a);
    fs::file* b = fs::open("/ino_b", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(b);

    fs::vattr attr_a = {};
    fs::vattr attr_b = {};
    EXPECT_EQ(fs::fstat(a, &attr_a), fs::OK);
    EXPECT_EQ(fs::fstat(b, &attr_b), fs::OK);
    EXPECT_NE(attr_a.ino, static_cast<uint64_t>(0));
    EXPECT_NE(attr_a.ino, attr_b.ino);
    EXPECT_EQ(attr_a.dev, attr_b.dev);

    fs::close(a);
    fs::close(b);
    fs::unlink("/ino_a");
    fs::unlink("/ino_b");
}

TEST(fs_test, stat_identity_stable_for_node) {
    fs::file* f = fs::open("/ino_stable", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    fs::vattr by_fd = {};
    fs::vattr by_path = {};
    EXPECT_EQ(fs::fstat(f, &by_fd), fs::OK);
    EXPECT_EQ(fs::stat("/ino_stable", &by_path), fs::OK);
    EXPECT_EQ(by_fd.ino, by_path.ino);
    EXPECT_EQ(by_fd.dev, by_path.dev);

    fs::close(f);
    fs::unlink("/ino_stable");
}

TEST(fs_test, stat_identity_distinct_per_filesystem) {
    fs::vattr root = {};
    fs::vattr dev = {};
    EXPECT_EQ(fs::stat("/", &root), fs::OK);
    EXPECT_EQ(fs::stat("/dev", &dev), fs::OK);
    EXPECT_NE(root.dev, static_cast<uint64_t>(0));
    EXPECT_NE(dev.dev, static_cast<uint64_t>(0));
    EXPECT_NE(root.dev, dev.dev);
}

TEST(fs_test, readdir_reports_node_identity) {
    fs::mkdir("/ino_dir", 0);
    fs::file* f = fs::open("/ino_dir/child", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat("/ino_dir/child", &attr), fs::OK);

    fs::file* dir = fs::open("/ino_dir", fs::O_RDONLY);
    ASSERT_NOT_NULL(dir);
    fs::dirent entry = {};
    EXPECT_EQ(fs::readdir(dir, &entry, 1), static_cast<ssize_t>(1));
    EXPECT_EQ(entry.ino, attr.ino);

    fs::close(dir);
    fs::unlink("/ino_dir/child");
    fs::rmdir("/ino_dir");
}

TEST(fs_test, timestamps_start_equal_and_nonzero) {
    fs::file* f = fs::open("/ts_new", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    fs::vattr attr = {};
    EXPECT_EQ(fs::fstat(f, &attr), fs::OK);
    EXPECT_NE(attr.mtime_ns, static_cast<uint64_t>(0));
    EXPECT_EQ(attr.atime_ns, attr.mtime_ns);
    EXPECT_EQ(attr.ctime_ns, attr.mtime_ns);

    fs::close(f);
    fs::unlink("/ts_new");
}

TEST(fs_test, setattr_applies_times_and_bumps_ctime) {
    fs::file* f = fs::open("/ts_set", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    fs::vattr before = {};
    EXPECT_EQ(fs::fstat(f, &before), fs::OK);

    fs::vattr want = {};
    want.atime_ns = 1000;
    want.mtime_ns = 2000;
    EXPECT_EQ(fs::fsetattr(f, want, fs::VATTR_ATIME | fs::VATTR_MTIME), fs::OK);

    fs::vattr after = {};
    EXPECT_EQ(fs::fstat(f, &after), fs::OK);
    EXPECT_EQ(after.atime_ns, static_cast<uint64_t>(1000));
    EXPECT_EQ(after.mtime_ns, static_cast<uint64_t>(2000));
    EXPECT_GE(after.ctime_ns, before.ctime_ns);

    fs::close(f);
    fs::unlink("/ts_set");
}

TEST(fs_test, setattr_rejects_unknown_mask) {
    fs::file* f = fs::open("/ts_mask", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    fs::vattr attr = {};
    EXPECT_EQ(fs::fsetattr(f, attr, 1u << 31), fs::ERR_INVAL);

    fs::close(f);
    fs::unlink("/ts_mask");
}

TEST(fs_test, write_moves_mtime_forward) {
    fs::file* f = fs::open("/ts_write", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    fs::vattr past = {};
    past.mtime_ns = 1;
    EXPECT_EQ(fs::fsetattr(f, past, fs::VATTR_MTIME), fs::OK);
    EXPECT_EQ(fs::write(f, "x", 1), static_cast<ssize_t>(1));

    fs::vattr attr = {};
    EXPECT_EQ(fs::fstat(f, &attr), fs::OK);
    EXPECT_GT(attr.mtime_ns, static_cast<uint64_t>(1));
    EXPECT_EQ(attr.ctime_ns, attr.mtime_ns);

    fs::close(f);
    fs::unlink("/ts_write");
}

TEST(fs_test, directory_mtime_tracks_entries) {
    EXPECT_EQ(fs::mkdir("/ts_dir", 0), fs::OK);
    fs::file* dir = fs::open("/ts_dir", fs::O_RDONLY);
    ASSERT_NOT_NULL(dir);

    fs::vattr past = {};
    past.mtime_ns = 1;
    EXPECT_EQ(fs::fsetattr(dir, past, fs::VATTR_MTIME), fs::OK);

    fs::file* child = fs::open("/ts_dir/child", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(child);
    fs::close(child);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat("/ts_dir", &attr), fs::OK);
    EXPECT_GT(attr.mtime_ns, static_cast<uint64_t>(1));

    fs::close(dir);
    fs::unlink("/ts_dir/child");
    fs::rmdir("/ts_dir");
}

TEST(fs_test, open_trunc_empties_writable_file) {
    fs::file* f = fs::open("/trunc_rw", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(fs::write(f, "0123456789", 10), static_cast<ssize_t>(10));
    fs::close(f);

    f = fs::open("/trunc_rw", fs::O_WRONLY | fs::O_TRUNC);
    ASSERT_NOT_NULL(f);

    fs::vattr attr = {};
    EXPECT_EQ(fs::fstat(f, &attr), fs::OK);
    EXPECT_EQ(attr.size, static_cast<size_t>(0));

    fs::close(f);
    fs::unlink("/trunc_rw");
}

TEST(fs_test, open_trunc_ignored_for_read_only) {
    fs::file* f = fs::open("/trunc_ro", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(fs::write(f, "0123456789", 10), static_cast<ssize_t>(10));
    fs::close(f);

    f = fs::open("/trunc_ro", fs::O_RDONLY | fs::O_TRUNC);
    ASSERT_NOT_NULL(f);

    fs::vattr attr = {};
    EXPECT_EQ(fs::fstat(f, &attr), fs::OK);
    EXPECT_EQ(attr.size, static_cast<size_t>(10));

    fs::close(f);
    fs::unlink("/trunc_ro");
}

TEST(fs_test, rename_in_place_keeps_identity) {
    fs::file* f = fs::open("/rn_a", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    fs::vattr before = {};
    EXPECT_EQ(fs::stat("/rn_a", &before), fs::OK);
    EXPECT_EQ(fs::rename("/rn_a", "/rn_b"), fs::OK);

    fs::vattr after = {};
    EXPECT_EQ(fs::stat("/rn_b", &after), fs::OK);
    EXPECT_EQ(after.ino, before.ino);
    EXPECT_GE(after.ctime_ns, before.ctime_ns);
    EXPECT_EQ(fs::stat("/rn_a", &before), fs::ERR_NOENT);

    fs::unlink("/rn_b");
}

TEST(fs_test, rename_across_directories_moves_content) {
    EXPECT_EQ(fs::mkdir("/rn_src", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/rn_dst", 0), fs::OK);
    fs::file* f = fs::open("/rn_src/f", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(fs::write(f, "moved", 5), static_cast<ssize_t>(5));
    fs::close(f);

    EXPECT_EQ(fs::rename("/rn_src/f", "/rn_dst/g"), fs::OK);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat("/rn_src", &attr), fs::OK);
    EXPECT_EQ(attr.size, static_cast<size_t>(0));
    EXPECT_EQ(fs::stat("/rn_dst", &attr), fs::OK);
    EXPECT_EQ(attr.size, static_cast<size_t>(1));

    f = fs::open("/rn_dst/g", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);
    char buf[8] = {};
    EXPECT_EQ(fs::read(f, buf, sizeof(buf)), static_cast<ssize_t>(5));
    EXPECT_STREQ(buf, "moved");
    fs::close(f);

    fs::unlink("/rn_dst/g");
    fs::rmdir("/rn_src");
    fs::rmdir("/rn_dst");
}

TEST(fs_test, rename_replaces_existing_file) {
    fs::file* f = fs::open("/rn_new", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(fs::write(f, "new", 3), static_cast<ssize_t>(3));
    fs::close(f);
    f = fs::open("/rn_old", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(fs::write(f, "old", 3), static_cast<ssize_t>(3));
    fs::close(f);

    EXPECT_EQ(fs::rename("/rn_new", "/rn_old"), fs::OK);

    f = fs::open("/rn_old", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);
    char buf[8] = {};
    EXPECT_EQ(fs::read(f, buf, sizeof(buf)), static_cast<ssize_t>(3));
    EXPECT_STREQ(buf, "new");
    fs::close(f);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat("/rn_new", &attr), fs::ERR_NOENT);

    fs::unlink("/rn_old");
}

TEST(fs_test, rename_onto_itself_is_noop) {
    fs::file* f = fs::open("/rn_self", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    EXPECT_EQ(fs::rename("/rn_self", "/rn_self"), fs::OK);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat("/rn_self", &attr), fs::OK);

    fs::unlink("/rn_self");
}

TEST(fs_test, rename_refuses_directory_into_own_subtree) {
    EXPECT_EQ(fs::mkdir("/rn_outer", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/rn_outer/inner", 0), fs::OK);

    EXPECT_EQ(fs::rename("/rn_outer", "/rn_outer/inner/x"), fs::ERR_INVAL);
    EXPECT_EQ(fs::rename("/rn_outer", "/rn_outer/x"), fs::ERR_INVAL);

    fs::rmdir("/rn_outer/inner");
    fs::rmdir("/rn_outer");
}

TEST(fs_test, rename_directory_over_nonempty_fails) {
    EXPECT_EQ(fs::mkdir("/rn_d1", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/rn_d2", 0), fs::OK);
    EXPECT_EQ(fs::mkdir("/rn_d2/x", 0), fs::OK);

    EXPECT_EQ(fs::rename("/rn_d1", "/rn_d2"), fs::ERR_NOTEMPTY);

    fs::rmdir("/rn_d2/x");
    EXPECT_EQ(fs::rename("/rn_d1", "/rn_d2"), fs::OK);

    fs::rmdir("/rn_d2");
}

TEST(fs_test, rename_type_mismatch_fails) {
    fs::file* f = fs::open("/rn_file", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);
    EXPECT_EQ(fs::mkdir("/rn_dir", 0), fs::OK);

    EXPECT_EQ(fs::rename("/rn_file", "/rn_dir"), fs::ERR_ISDIR);
    EXPECT_EQ(fs::rename("/rn_dir", "/rn_file"), fs::ERR_NOTDIR);

    fs::unlink("/rn_file");
    fs::rmdir("/rn_dir");
}

TEST(fs_test, rename_across_filesystems_fails) {
    fs::file* f = fs::open("/rn_xdev", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    EXPECT_EQ(fs::rename("/rn_xdev", "/dev/rn_xdev"), fs::ERR_XDEV);

    fs::unlink("/rn_xdev");
}

TEST(fs_test, open_excl_creates_only_new_files) {
    int32_t err = fs::OK;
    fs::file* f = fs::open("/excl_new", fs::O_CREAT | fs::O_EXCL | fs::O_RDWR, &err);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(err, fs::OK);
    fs::close(f);

    f = fs::open("/excl_new", fs::O_CREAT | fs::O_EXCL | fs::O_RDWR, &err);
    EXPECT_NULL(f);
    EXPECT_EQ(err, fs::ERR_EXIST);

    f = fs::open("/excl_new", fs::O_CREAT | fs::O_RDWR, &err);
    EXPECT_NOT_NULL(f);
    EXPECT_EQ(err, fs::OK);
    fs::close(f);

    fs::unlink("/excl_new");
}

TEST(fs_test, symlink_stat_follows_and_lookup_nofollow_does_not) {
    fs::file* f = fs::open("/sl_target", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(fs::write(f, "hello", 5), static_cast<ssize_t>(5));
    fs::close(f);

    EXPECT_EQ(fs::symlink("/sl_target", "/sl_link"), fs::OK);

    fs::vattr target = {};
    fs::vattr through = {};
    EXPECT_EQ(fs::stat("/sl_target", &target), fs::OK);
    EXPECT_EQ(fs::stat("/sl_link", &through), fs::OK);
    EXPECT_EQ(through.type, fs::node_type::regular);
    EXPECT_EQ(through.ino, target.ino);
    EXPECT_EQ(through.size, static_cast<size_t>(5));

    fs::node* link = nullptr;
    ASSERT_EQ(fs::lookup_at(nullptr, "/sl_link", fs::LOOKUP_NOFOLLOW, &link), fs::OK);
    fs::vattr self = {};
    EXPECT_EQ(link->getattr(&self), fs::OK);
    EXPECT_EQ(self.type, fs::node_type::symlink);
    EXPECT_EQ(self.size, static_cast<size_t>(10));
    EXPECT_NE(self.ino, target.ino);

    char buf[32] = {};
    size_t len = 0;
    EXPECT_EQ(link->readlink(buf, sizeof(buf), &len), fs::OK);
    EXPECT_EQ(len, static_cast<size_t>(10));
    EXPECT_STREQ(buf, "/sl_target");
    release_node(link);

    fs::unlink("/sl_link");
    fs::unlink("/sl_target");
}

TEST(fs_test, symlink_open_reads_target_content) {
    fs::file* f = fs::open("/sl_data", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(fs::write(f, "via link", 8), static_cast<ssize_t>(8));
    fs::close(f);
    EXPECT_EQ(fs::symlink("sl_data", "/sl_rel"), fs::OK);

    f = fs::open("/sl_rel", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);
    char buf[16] = {};
    EXPECT_EQ(fs::read(f, buf, sizeof(buf)), static_cast<ssize_t>(8));
    EXPECT_STREQ(buf, "via link");
    fs::close(f);

    fs::unlink("/sl_rel");
    fs::unlink("/sl_data");
}

TEST(fs_test, symlink_to_directory_in_the_middle_of_a_path) {
    EXPECT_EQ(fs::mkdir("/sl_real", 0), fs::OK);
    fs::file* f = fs::open("/sl_real/inner", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);
    EXPECT_EQ(fs::symlink("/sl_real", "/sl_dirlink"), fs::OK);

    fs::vattr direct = {};
    fs::vattr via = {};
    EXPECT_EQ(fs::stat("/sl_real/inner", &direct), fs::OK);
    EXPECT_EQ(fs::stat("/sl_dirlink/inner", &via), fs::OK);
    EXPECT_EQ(via.ino, direct.ino);
    EXPECT_EQ(fs::stat("/sl_dirlink/../sl_real/inner", &via), fs::OK);
    EXPECT_EQ(via.ino, direct.ino);

    fs::unlink("/sl_dirlink");
    fs::unlink("/sl_real/inner");
    fs::rmdir("/sl_real");
}

TEST(fs_test, symlink_loop_and_dangling_targets) {
    EXPECT_EQ(fs::symlink("/sl_loop_b", "/sl_loop_a"), fs::OK);
    EXPECT_EQ(fs::symlink("/sl_loop_a", "/sl_loop_b"), fs::OK);
    EXPECT_EQ(fs::symlink("/sl_missing", "/sl_dangling"), fs::OK);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat("/sl_loop_a", &attr), fs::ERR_LOOP);
    EXPECT_EQ(fs::stat("/sl_dangling", &attr), fs::ERR_NOENT);

    fs::node* link = nullptr;
    EXPECT_EQ(fs::lookup_at(nullptr, "/sl_dangling", fs::LOOKUP_NOFOLLOW, &link), fs::OK);
    release_node(link);

    fs::unlink("/sl_loop_a");
    fs::unlink("/sl_loop_b");
    fs::unlink("/sl_dangling");
}

// Writes "/sl_chain<index>" with two decimal digits into out
static void chain_link_name(uint32_t index, char* out) {
    const char prefix[] = "/sl_chain";
    string::memcpy(out, prefix, sizeof(prefix) - 1);
    out[sizeof(prefix) - 1] = static_cast<char>('0' + index / 10);
    out[sizeof(prefix)] = static_cast<char>('0' + index % 10);
    out[sizeof(prefix) + 1] = '\0';
}

// Builds /sl_chain<links-1> -> ... -> /sl_chain00 -> /sl_chain_end
static void make_symlink_chain(uint32_t links, char* head) {
    fs::file* f = fs::open("/sl_chain_end", fs::O_CREAT | fs::O_RDWR);
    if (f) {
        fs::close(f);
    }

    char target[32] = "/sl_chain_end";
    for (uint32_t i = 0; i < links; i++) {
        chain_link_name(i, head);
        fs::symlink(target, head);
        string::memcpy(target, head, string::strlen(head) + 1);
    }
}

static void remove_symlink_chain(uint32_t links) {
    fs::unlink("/sl_chain_end");

    char name[32];
    for (uint32_t i = 0; i < links; i++) {
        chain_link_name(i, name);
        fs::unlink(name);
    }
}

TEST(fs_test, symlink_chain_within_limit_resolves) {
    char head[32];
    make_symlink_chain(5, head);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat(head, &attr), fs::OK);
    EXPECT_EQ(attr.type, fs::node_type::regular);

    remove_symlink_chain(5);
}

TEST(fs_test, symlink_chain_beyond_limit_fails) {
    char head[32];
    make_symlink_chain(fs::SYMLOOP_MAX + 1, head);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat(head, &attr), fs::ERR_LOOP);

    remove_symlink_chain(fs::SYMLOOP_MAX + 1);
}

TEST(fs_test, symlink_unlink_and_rename_act_on_the_link) {
    fs::file* f = fs::open("/sl_kept", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);
    EXPECT_EQ(fs::symlink("/sl_kept", "/sl_l1"), fs::OK);

    EXPECT_EQ(fs::rename("/sl_l1", "/sl_l2"), fs::OK);
    fs::node* link = nullptr;
    ASSERT_EQ(fs::lookup_at(nullptr, "/sl_l2", fs::LOOKUP_NOFOLLOW, &link), fs::OK);
    EXPECT_EQ(link->type(), fs::node_type::symlink);
    release_node(link);

    EXPECT_EQ(fs::unlink("/sl_l2"), fs::OK);
    fs::vattr attr = {};
    EXPECT_EQ(fs::stat("/sl_kept", &attr), fs::OK);
    EXPECT_EQ(fs::stat("/sl_l2", &attr), fs::ERR_NOENT);

    fs::unlink("/sl_kept");
}

TEST(fs_test, symlink_rejects_existing_name_and_empty_target) {
    fs::file* f = fs::open("/sl_taken", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    EXPECT_EQ(fs::symlink("/anything", "/sl_taken"), fs::ERR_EXIST);
    EXPECT_EQ(fs::symlink("", "/sl_empty"), fs::ERR_INVAL);

    fs::unlink("/sl_taken");
}

TEST(fs_test, multi_page_write_read) {
    fs::file* f = fs::open("/bigfile", fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    constexpr size_t WRITE_SIZE = 8192;
    uint8_t wbuf[WRITE_SIZE];
    for (size_t i = 0; i < WRITE_SIZE; i++) {
        wbuf[i] = static_cast<uint8_t>(i & 0xFF);
    }

    ssize_t written = fs::write(f, wbuf, WRITE_SIZE);
    EXPECT_EQ(written, static_cast<ssize_t>(WRITE_SIZE));

    fs::seek(f, 0, fs::SEEK_SET);

    uint8_t rbuf[WRITE_SIZE];
    string::memset(rbuf, 0, WRITE_SIZE);
    ssize_t rd = fs::read(f, rbuf, WRITE_SIZE);
    EXPECT_EQ(rd, static_cast<ssize_t>(WRITE_SIZE));

    bool match = true;
    for (size_t i = 0; i < WRITE_SIZE; i++) {
        if (rbuf[i] != wbuf[i]) {
            match = false;
            break;
        }
    }
    EXPECT_TRUE(match);

    fs::close(f);
    fs::unlink("/bigfile");
}
