#include <unit_tests/unit_tests.h>
#include <fs/vfs.h>
#include <fs/ram_filesystem.h>

using namespace fs;

// Test mounting and unmounting a RAM filesystem
DECLARE_UNIT_TEST("vfs mount/unmount", test_vfs_mount_unmount) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    bool root_path_exists = vfs.path_exists("/");
    ASSERT_TRUE(root_path_exists, "\"/\" path does not exist");

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test creating a directory
DECLARE_UNIT_TEST("vfs create directory", test_vfs_create_directory) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string dir_path = "/home";
    status = vfs.create(dir_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir_path.c_str(), error_to_string(status));

    bool path_exists = vfs.path_exists(dir_path);
    ASSERT_TRUE(path_exists, "Directory '%s' does not exist after creation", dir_path.c_str());

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test creating a file
DECLARE_UNIT_TEST("vfs create file", test_vfs_create_file) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string file_path = "/test_file.txt";
    status = vfs.create(file_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file_path.c_str(), error_to_string(status));

    bool path_exists = vfs.path_exists(file_path);
    ASSERT_TRUE(path_exists, "File '%s' does not exist after creation", file_path.c_str());

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test writing to a file
DECLARE_UNIT_TEST("vfs write to file", test_vfs_write_file) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string file_path = "/write_test.txt";
    status = vfs.create(file_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file_path.c_str(), error_to_string(status));

    const char* write_data = "Hello, VFS!";
    ssize_t bytes_written = vfs.write(file_path, write_data, strlen(write_data), 0);
    ASSERT_EQ(bytes_written, (ssize_t)strlen(write_data), "Failed to write to file '%s': Expected %lli bytes, wrote %lli bytes", 
                        file_path.c_str(), (long long)strlen(write_data), (long long)bytes_written);

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test reading from a file
DECLARE_UNIT_TEST("vfs read from file", test_vfs_read_file) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string file_path = "/read_test.txt";
    status = vfs.create(file_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file_path.c_str(), error_to_string(status));

    const char* write_data = "Read this data.";
    ssize_t bytes_written = vfs.write(file_path, write_data, strlen(write_data), 0);
    ASSERT_EQ(bytes_written, (ssize_t)strlen(write_data), "Failed to write to file '%s': Expected %lli bytes, wrote %lli bytes", 
                        file_path.c_str(), (long long)strlen(write_data), (long long)bytes_written);

    char buffer[32] = {0};
    ssize_t bytes_read = vfs.read(file_path, buffer, sizeof(buffer) - 1, 0);
    ASSERT_EQ(bytes_read, (ssize_t)strlen(write_data), "Failed to read from file '%s': Expected %lli bytes, read %lli bytes", 
                        file_path.c_str(), (long long)strlen(write_data), (long long)bytes_read);
    ASSERT_EQ(strcmp(buffer, write_data), 0, "Data read from file '%s' does not match data written", file_path.c_str());

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test listing directory contents
DECLARE_UNIT_TEST("vfs list directory", test_vfs_list_directory) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    // Create directories and files
    kstl::string dir_path = "/dir1";
    status = vfs.create(dir_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir_path.c_str(), error_to_string(status));

    kstl::string file1_path = "/dir1/file1.txt";
    status = vfs.create(file1_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file1_path.c_str(), error_to_string(status));

    kstl::string file2_path = "/dir1/file2.txt";
    status = vfs.create(file2_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file2_path.c_str(), error_to_string(status));

    // List contents of /dir1
    kstl::vector<kstl::string> entries;
    status = vfs.listdir(dir_path, entries);
    ASSERT_EQ(status, fs_error::success, "Failed to list directory '%s': %s", dir_path.c_str(), error_to_string(status));
    ASSERT_EQ(entries.size(), 2, "Directory '%s' should contain 2 entries, found %llu", dir_path.c_str(), entries.size());

    // Verify contents
    bool found_file1 = false;
    bool found_file2 = false;
    for (const auto& entry : entries) {
        if (entry == "file1.txt") {
            found_file1 = true;
        } else if (entry == "file2.txt") {
            found_file2 = true;
        }
    }
    ASSERT_TRUE(found_file1, "'file1.txt' not found in directory '%s'", dir_path.c_str());
    ASSERT_TRUE(found_file2, "'file2.txt' not found in directory '%s'", dir_path.c_str());

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test the `stat()` functionality on a file
DECLARE_UNIT_TEST("vfs stat file", test_vfs_stat_file) {
    // Create and mount a RAM filesystem
    auto mockfs = kstl::make_shared<ram_filesystem>();
    auto& vfs = virtual_filesystem::get();

    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    // Create a file
    kstl::string file_path = "/stat_test.txt";
    status = vfs.create(file_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s",
              file_path.c_str(), error_to_string(status));

    // Write some data
    const char* write_data = "Check stat!";
    ssize_t bytes_written = vfs.write(file_path, write_data, strlen(write_data), 0);
    ASSERT_EQ(bytes_written, (ssize_t)strlen(write_data),
              "Failed to write to file '%s': Expected %lli bytes, wrote %lli bytes",
              file_path.c_str(), (long long)strlen(write_data), (long long)bytes_written);

    // 4) Call stat() on the newly created file
    vfs_stat_struct info;
    status = vfs.stat(file_path, info);
    ASSERT_EQ(status, fs_error::success, "Failed to stat '%s': %s",
              file_path.c_str(), error_to_string(status));

    // 5) Check that file metadata is correct
    //    Adjust the checks based on what your vfs_stat_struct contains.
    ASSERT_EQ(info.type, vfs_node_type::file,
              "Expected node type 'file' but got something else");
    ASSERT_EQ(info.size, (uint64_t)strlen(write_data),
              "Expected file size %llu, got %llu",
              strlen(write_data), (unsigned long long)info.size);

    // Permissions may be stored differently; adjust as needed.
    ASSERT_EQ(info.perms, 0644,
              "Expected file permissions 0644, got %u", info.perms);

    // Unmount and finish
    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test removing a file
DECLARE_UNIT_TEST("vfs remove file", test_vfs_remove_file) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string file_path = "/remove_test.txt";
    status = vfs.create(file_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file_path.c_str(), error_to_string(status));

    bool path_exists = vfs.path_exists(file_path);
    ASSERT_TRUE(path_exists, "File '%s' does not exist after creation", file_path.c_str());

    // Remove the file
    status = vfs.remove(file_path);
    ASSERT_EQ(status, fs_error::success, "Failed to remove file '%s': %s", file_path.c_str(), error_to_string(status));

    // Verify removal
    path_exists = vfs.path_exists(file_path);
    ASSERT_TRUE(!path_exists, "File '%s' still exists after removal", file_path.c_str());

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test removing a directory
DECLARE_UNIT_TEST("vfs remove directory", test_vfs_remove_directory) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string dir_path = "/remove_dir";
    status = vfs.create(dir_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir_path.c_str(), error_to_string(status));

    bool path_exists = vfs.path_exists(dir_path);
    ASSERT_TRUE(path_exists, "Directory '%s' does not exist after creation", dir_path.c_str());

    // Remove the directory
    status = vfs.remove(dir_path);
    ASSERT_EQ(status, fs_error::success, "Failed to remove directory '%s': %s", dir_path.c_str(), error_to_string(status));

    // Verify removal
    path_exists = vfs.path_exists(dir_path);
    ASSERT_TRUE(!path_exists, "Directory '%s' still exists after removal", dir_path.c_str());

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test attempting to create an existing directory
DECLARE_UNIT_TEST("vfs create existing directory", test_vfs_create_existing_directory) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string dir_path = "/existing_dir";
    status = vfs.create(dir_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir_path.c_str(), error_to_string(status));

    // Attempt to create the same directory again
    status = vfs.create(dir_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::already_exists, "Creating existing directory '%s' should fail with 'already_exists', got: %s", 
                        dir_path.c_str(), error_to_string(status));

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test attempting to remove a non-existing file
DECLARE_UNIT_TEST("vfs remove non-existing file", test_vfs_remove_nonexisting_file) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string file_path = "/nonexistent_file.txt";

    // Attempt to remove a file that doesn't exist
    status = vfs.remove(file_path);
    ASSERT_EQ(status, fs_error::not_found, "Removing non-existing file '%s' should fail with 'not_found', got: %s", 
                        file_path.c_str(), error_to_string(status));

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test creating nested directories one by one
DECLARE_UNIT_TEST("vfs create nested directories", test_vfs_create_nested_directories) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    // Create /dir1
    kstl::string dir1_path = "/dir1";
    status = vfs.create(dir1_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir1_path.c_str(), error_to_string(status));

    bool path_exists = vfs.path_exists(dir1_path);
    ASSERT_TRUE(path_exists, "Directory '%s' does not exist after creation", dir1_path.c_str());

    // Create /dir1/dir2
    kstl::string dir2_path = "/dir1/dir2";
    status = vfs.create(dir2_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir2_path.c_str(), error_to_string(status));

    path_exists = vfs.path_exists(dir2_path);
    ASSERT_TRUE(path_exists, "Directory '%s' does not exist after creation", dir2_path.c_str());

    // Create /dir1/dir2/dir3
    kstl::string dir3_path = "/dir1/dir2/dir3";
    status = vfs.create(dir3_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir3_path.c_str(), error_to_string(status));

    path_exists = vfs.path_exists(dir3_path);
    ASSERT_TRUE(path_exists, "Directory '%s' does not exist after creation", dir3_path.c_str());

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test removing a directory that contains files and subdirectories
DECLARE_UNIT_TEST("vfs remove directory with contents", test_vfs_remove_directory_with_contents) {
    // Step 1: Mount the RAM filesystem
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    // Create a directory
    kstl::string dir_path = "/dir_with_contents";
    status = vfs.create(dir_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir_path.c_str(), error_to_string(status));

    // Add contents to the directory
    // Create files inside the directory
    kstl::string file1_path = "/dir_with_contents/file1.txt";
    status = vfs.create(file1_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file1_path.c_str(), error_to_string(status));

    kstl::string file2_path = "/dir_with_contents/file2.txt";
    status = vfs.create(file2_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file2_path.c_str(), error_to_string(status));

    // Create a subdirectory inside the directory
    kstl::string subdir_path = "/dir_with_contents/subdir";
    status = vfs.create(subdir_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create subdirectory '%s': %s", subdir_path.c_str(), error_to_string(status));

    // Create a file inside the subdirectory
    kstl::string subdir_file_path = "/dir_with_contents/subdir/file3.txt";
    status = vfs.create(subdir_file_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", subdir_file_path.c_str(), error_to_string(status));

    // Remove the directory (should recursively remove all contents)
    status = vfs.remove(dir_path);
    ASSERT_EQ(status, fs_error::success, "Failed to remove directory '%s': %s", dir_path.c_str(), error_to_string(status));

    // Verify that the directory no longer exists
    ASSERT_TRUE(!vfs.path_exists(dir_path), "Directory '%s' still exists after removal", dir_path.c_str());

    // Verify that all nested contents no longer exist
    ASSERT_TRUE(!vfs.path_exists(file1_path), "File '%s' still exists after removing directory", file1_path.c_str());
    ASSERT_TRUE(!vfs.path_exists(file2_path), "File '%s' still exists after removing directory", file2_path.c_str());
    ASSERT_TRUE(!vfs.path_exists(subdir_path), "Subdirectory '%s' still exists after removing directory", subdir_path.c_str());
    ASSERT_TRUE(!vfs.path_exists(subdir_file_path), "File '%s' still exists after removing directory", subdir_file_path.c_str());

    // Unmount the filesystem
    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test reading from a non-existing file
DECLARE_UNIT_TEST("vfs read non-existing file", test_vfs_read_nonexisting_file) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string file_path = "/nonexistent_read.txt";
    char buffer[16] = {0};
    ssize_t bytes_read = vfs.read(file_path, buffer, sizeof(buffer) - 1, 0);
    ASSERT_EQ(bytes_read, (ssize_t)fs_error::not_found, "Reading non-existing file '%s' should fail with 'not_found', got: %lli", 
                        file_path.c_str(), (long long)bytes_read);

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test writing to a non-existing file
DECLARE_UNIT_TEST("vfs write non-existing file", test_vfs_write_nonexisting_file) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string file_path = "/nonexistent_write.txt";
    const char* write_data = "Attempt to write";
    ssize_t bytes_written = vfs.write(file_path, write_data, strlen(write_data), 0);
    ASSERT_EQ(bytes_written, (ssize_t)fs_error::not_found, "Writing to non-existing file '%s' should fail with 'not_found', got: %lli", 
                        file_path.c_str(), (long long)bytes_written);

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test listing contents of a non-existing directory
DECLARE_UNIT_TEST("vfs list non-existing directory", test_vfs_list_nonexisting_directory) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string dir_path = "/nonexistent_dir";
    kstl::vector<kstl::string> entries;
    status = vfs.listdir(dir_path, entries);
    ASSERT_EQ(status, fs_error::not_found, "Listing non-existing directory '%s' should fail with 'not_found', got: %s", 
                        dir_path.c_str(), error_to_string(status));

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test path existence for various paths
DECLARE_UNIT_TEST("vfs path existence", test_vfs_path_existence) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    // Initial paths
    ASSERT_TRUE(vfs.path_exists("/"), "\"/\" path should exist after mounting");
    ASSERT_TRUE(!vfs.path_exists("/home"), "\"/home\" path should not exist initially");

    // Create directory
    kstl::string dir_path = "/home";
    status = vfs.create(dir_path, fs::vfs_node_type::directory, 0755);
    ASSERT_EQ(status, fs_error::success, "Failed to create directory '%s': %s", dir_path.c_str(), error_to_string(status));

    // Check existence
    ASSERT_TRUE(vfs.path_exists(dir_path), "\"%s\" path should exist after creation", dir_path.c_str());

    // Create file
    kstl::string file_path = "/home/file.txt";
    status = vfs.create(file_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file_path.c_str(), error_to_string(status));

    // Check existence
    ASSERT_TRUE(vfs.path_exists(file_path), "\"%s\" path should exist after creation", file_path.c_str());

    // Check non-existing paths
    ASSERT_TRUE(!vfs.path_exists("/nonexistent"), "\"/nonexistent\" path should not exist");
    ASSERT_TRUE(!vfs.path_exists("/home/nonexistent_file.txt"), "\"/home/nonexistent_file.txt\" path should not exist");

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test creating and removing multiple files
DECLARE_UNIT_TEST("vfs create and remove multiple files", test_vfs_create_remove_multiple_files) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    // Create multiple files
    kstl::vector<kstl::string> file_paths;
    file_paths.push_back("/file1.txt");
    file_paths.push_back("/file2.txt");
    file_paths.push_back("/file3.txt");
    file_paths.push_back("/file4.txt");
    file_paths.push_back("/file5.txt");

    for (const auto& path : file_paths) {
        status = vfs.create(path, fs::vfs_node_type::file, 0644);
        ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", path.c_str(), error_to_string(status));
        ASSERT_TRUE(vfs.path_exists(path), "File '%s' does not exist after creation", path.c_str());
    }

    // Remove the files
    for (const auto& path : file_paths) {
        status = vfs.remove(path);
        ASSERT_EQ(status, fs_error::success, "Failed to remove file '%s': %s", path.c_str(), error_to_string(status));
        ASSERT_TRUE(!vfs.path_exists(path), "File '%s' still exists after removal", path.c_str());
    }

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}

// Test creating a file, writing to it, and verifying contents
DECLARE_UNIT_TEST("vfs create write and verify file contents", test_vfs_write_verify_file) {
    auto mockfs = kstl::make_shared<ram_filesystem>();

    auto& vfs = virtual_filesystem::get();
    fs_error status = vfs.mount("/", mockfs);
    ASSERT_EQ(status, fs_error::success, "Failed to mount ramfs: %s", error_to_string(status));

    kstl::string file_path = "/data.txt";
    status = vfs.create(file_path, fs::vfs_node_type::file, 0644);
    ASSERT_EQ(status, fs_error::success, "Failed to create file '%s': %s", file_path.c_str(), error_to_string(status));

    const char* write_data = "Unit Test Data";
    ssize_t bytes_written = vfs.write(file_path, write_data, strlen(write_data), 0);
    ASSERT_EQ(bytes_written, (ssize_t)strlen(write_data), "Failed to write to file '%s': Expected %lli bytes, wrote %lli bytes", 
                        file_path.c_str(), (long long)strlen(write_data), (long long)bytes_written);

    char buffer[32] = {0};
    ssize_t bytes_read = vfs.read(file_path, buffer, sizeof(buffer) - 1, 0);
    ASSERT_EQ(bytes_read, (ssize_t)strlen(write_data), "Failed to read from file '%s': Expected %lli bytes, read %lli bytes", 
                        file_path.c_str(), (long long)strlen(write_data), (long long)bytes_read);
    ASSERT_EQ(strcmp(buffer, write_data), 0, "Data read from file '%s' does not match data written", file_path.c_str());

    status = vfs.unmount("/");
    ASSERT_EQ(status, fs_error::success, "Failed to unmount ramfs: %s", error_to_string(status));

    return UNIT_TEST_SUCCESS;
}
