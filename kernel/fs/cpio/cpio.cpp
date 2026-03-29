#include "fs/cpio/cpio.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "boot/boot_services.h"
#include "mm/vmm.h"
#include "mm/paging.h"
#include "common/string.h"
#include "common/logging.h"

namespace cpio {

constexpr char CPIO_MAGIC[] = "070701";
constexpr char CPIO_TRAILER[] = "TRAILER!!!";
constexpr uint32_t S_IFMT  = 0170000;
constexpr uint32_t S_IFDIR = 0040000;
constexpr uint32_t S_IFREG = 0100000;

__PRIVILEGED_CODE static uint32_t hex_to_u32(const char* s, size_t len) {
    uint32_t val = 0;
    for (size_t i = 0; i < len; i++) {
        val <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9') {
            val |= static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val |= static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            val |= static_cast<uint32_t>(c - 'A' + 10);
        }
    }
    return val;
}

__PRIVILEGED_CODE static size_t align4(size_t x) {
    return (x + 3) & ~static_cast<size_t>(3);
}

__PRIVILEGED_CODE static bool has_dotdot(const char* path) {
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '.' && path[i + 1] == '.') {
            if (i == 0 || path[i - 1] == '/') {
                if (path[i + 2] == '\0' || path[i + 2] == '/') {
                    return true;
                }
            }
        }
    }
    return false;
}

static void ensure_parents(const char* path) {
    char buf[fs::PATH_MAX];
    size_t len = string::strnlen(path, fs::PATH_MAX - 1);
    string::memcpy(buf, path, len);
    buf[len] = '\0';

    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            fs::mkdir(buf, 0);
            buf[i] = '/';
        }
    }
}

__PRIVILEGED_CODE int32_t load_initrd() {
    if (g_boot_info.module_count == 0) {
        return ERR_NO_MODULE;
    }

    uint64_t phys = g_boot_info.modules[0].phys_addr;
    uint64_t size = g_boot_info.modules[0].size;

    if (size == 0) {
        return ERR_NO_MODULE;
    }

    uintptr_t base_va = 0;
    uintptr_t usable_va = 0;
    int32_t rc = vmm::map_phys(phys, size, paging::PAGE_KERNEL_RW, base_va, usable_va);
    if (rc != vmm::OK) {
        log::error("cpio: failed to map module (phys=0x%lx size=%lu err=%d)", phys, size, rc);
        return ERR_MAP_FAILED;
    }

    const auto* archive = reinterpret_cast<const uint8_t*>(usable_va);
    size_t archive_len = static_cast<size_t>(size);

    uint32_t files_extracted = 0;
    uint32_t dirs_created = 0;
    size_t offset = 0;

    while (offset + sizeof(cpio_newc_header) <= archive_len) {
        const auto* hdr = reinterpret_cast<const cpio_newc_header*>(archive + offset);

        if (string::memcmp(hdr->c_magic, CPIO_MAGIC, 6) != 0) {
            break;
        }

        uint32_t namesize = hex_to_u32(hdr->c_namesize, 8);
        uint32_t filesize = hex_to_u32(hdr->c_filesize, 8);
        uint32_t mode     = hex_to_u32(hdr->c_mode, 8);

        offset += sizeof(cpio_newc_header);

        if (offset + namesize > archive_len) {
            break;
        }

        const char* filename = reinterpret_cast<const char*>(archive + offset);

        if (string::strcmp(filename, CPIO_TRAILER) == 0) {
            break;
        }

        offset += namesize;
        offset = align4(offset);

        const uint8_t* file_data = archive + offset;

        const char* name = filename;
        if (name[0] == '.' && name[1] == '/') {
            name += 2;
        } else if (name[0] == '/') {
            name += 1;
        }

        if (name[0] == '\0' || (name[0] == '.' && name[1] == '\0')) {
            offset += filesize;
            offset = align4(offset);
            continue;
        }

        if (has_dotdot(name)) {
            log::warn("cpio: rejecting path with '..': %s", name);
            offset += filesize;
            offset = align4(offset);
            continue;
        }

        constexpr size_t PREFIX_LEN = 1;
        char path_buf[fs::PATH_MAX];
        path_buf[0] = '/';

        size_t name_len = string::strnlen(name, fs::PATH_MAX - PREFIX_LEN - 1);
        string::memcpy(path_buf + PREFIX_LEN, name, name_len);
        path_buf[PREFIX_LEN + name_len] = '\0';

        bool is_dir = (mode & S_IFMT) == S_IFDIR;

        if (is_dir) {
            ensure_parents(path_buf);
            fs::mkdir(path_buf, 0);
            dirs_created++;
        } else if ((mode & S_IFMT) == S_IFREG) {
            ensure_parents(path_buf);
            fs::file* f = fs::open(path_buf, fs::O_CREAT | fs::O_WRONLY);
            if (f) {
                if (filesize > 0 && offset + filesize <= archive_len) {
                    fs::write(f, file_data, filesize);
                }
                fs::close(f);
                files_extracted++;
            } else {
                log::warn("cpio: failed to create file %s", path_buf);
            }
        }

        offset += filesize;
        offset = align4(offset);
    }

    vmm::free(base_va);

    log::info("cpio: extracted %u files and %u directories into /",
              files_extracted, dirs_created);
    return OK;
}

} // namespace cpio
