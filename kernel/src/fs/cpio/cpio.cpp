#include <fs/cpio/cpio.h>
#include <fs/vfs.h>
#include <fs/ram_filesystem.h>
#include <serial/serial.h>

namespace fs {
static kstl::string fixup_cpio_path(const char* cpio_name) {
    if (cpio_name[0] == '.' && cpio_name[1] == '/') {
        return kstl::string("/") + (cpio_name + 2);
    } else if (cpio_name[0] != '/') {
        // You might want a leading slash for everything
        return kstl::string("/") + cpio_name;
    }
    return kstl::string(cpio_name);
}

static __force_inline__ size_t align4(size_t x) {
    return (x + 3) & ~3UL;
}

uint32_t cpio_from_hex_str(const char* str, size_t len) {
    uint32_t val = 0;
    for (size_t i = 0; i < len; i++) {
        val <<= 4;
        char c = str[i];
        if (c >= '0' && c <= '9') {
            val |= (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val |= (c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            val |= (c - 'A' + 10);
        } else {
            // Handle error or just skip invalid characters
        }
    }
    return val;
}

bool cpio_is_dir(uint32_t mode) {
    return ((mode & 0170000) == 0040000); // octal 040000 => directory
}

__PRIVILEGED_CODE
void load_cpio_initrd(
    const uint8_t* cpio_archive,
    size_t length,
    const char* mount_path
) {
    auto ramfs = kstl::make_shared<ram_filesystem>();
    auto& vfs = virtual_filesystem::get();
    vfs.mount(mount_path, ramfs);

    fs_error mnt_status = vfs.mount(mount_path, ramfs);
    if (mnt_status != fs_error::success) {
        serial::printf("cpio: Failed to mount ramfs at '%s': %s\n",
            mount_path, error_to_string(mnt_status));
        return;
    }

    size_t offset = 0;
    while (true) {
        // Check if there's enough room for the header
        if (offset + sizeof(cpio_newc_header) > length) {
            serial::printf("cpio: No more room for header, done.\n");
            break; // End of archive or corrupt
        }

        // Interpret the header
        const cpio_newc_header* hdr =
            reinterpret_cast<const cpio_newc_header*>(cpio_archive + offset);

        // Check magic "070701"
        if (memcmp(hdr->c_magic, CPIO_HEADER_MAGIC, 6) != 0) {
            serial::printf("cpio: Magic mismatch or end of archive, done.\n");
            break;
        }

        // Parse relevant fields
        uint32_t namesize = cpio_from_hex_str(hdr->c_namesize, 8);
        uint32_t filesize = cpio_from_hex_str(hdr->c_filesize, 8);
        uint32_t mode     = cpio_from_hex_str(hdr->c_mode,     8);

        // Advance past the header
        offset += sizeof(cpio_newc_header);

        // Read the filename
        if (offset + namesize > length) {
            break;
        }
        const char* filename = reinterpret_cast<const char*>(cpio_archive + offset);

        // If the filename is "TRAILER!!!", it's the end
        if (!strcmp(filename, CPIO_TRAILER_MARK)) {
            break;
        }

        // Move offset by namesize and align
        offset += namesize;
        offset = align4(offset);

        // This is where file data starts (if it's not a directory)
        const uint8_t* file_data = (cpio_archive + offset);

        // Construct the final path within the mounted FS
        //    For example, if mount_path == "/initrd", and cpio name is "./flag.txt",
        //    fixup_cpio_path returns "/flag.txt", so final is "/initrd/flag.txt".
        kstl::string cpio_name = fixup_cpio_path(filename);

        // If mount_path ends with '/', watch out for double slashes. Typically, you'd do:
        kstl::string final_path = mount_path;
        if (final_path[final_path.length() - 1] != '/') {
            final_path += "/";
        }

        // Now combine
        final_path += (cpio_name.starts_with("/") ? cpio_name.substring(1) : cpio_name);

        bool is_dir = cpio_is_dir(mode);

        // Create the node in VFS
        fs_error create_status;
        if (is_dir) {
            create_status = vfs.create(final_path, vfs_node_type::directory, 0755);
            if (create_status == fs_error::success) {
                serial::printf("initrd: created directory %s\n", final_path.c_str());
            } else {
                serial::printf("initrd: failed to create directory %s -> %s\n",
                    final_path.c_str(), error_to_string(create_status));
            }
        } else {
            create_status = vfs.create(final_path, vfs_node_type::file, 0644);
            if (create_status == fs_error::success) {
                // Write file data
                if (filesize > 0) {
                    ssize_t written = vfs.write(final_path, file_data, filesize, 0);
                    if (written < 0) {
                        serial::printf("initrd: write to %s failed -> %s\n",
                            final_path.c_str(), error_to_string(written));
                    } else {
                        serial::printf("initrd: created file %s (size=%u)\n",
                            final_path.c_str(), filesize);
                    }
                } else {
                    serial::printf("initrd: created empty file %s\n", final_path.c_str());
                }
            } else {
                serial::printf("initrd: failed to create file %s -> %s\n",
                    final_path.c_str(), error_to_string(create_status));
            }
        }

        // Skip over the file content
        offset += filesize;
        offset = align4(offset);
    }

    serial::printf("[*] Successfully mounted initrd at: '%s'\n", mount_path);
}
} // namespace fs
