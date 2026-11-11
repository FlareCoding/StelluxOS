#define _POSIX_C_SOURCE 200809L

#include "install.h"
#include "desktop.h"
#include "files.h"
#include "records.h"
#include "remove.h"
#include "tar.h"

#include <bearssl/bearssl.h>
#include <stlxhttp/http.h>
#include <zstd.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STAGING_DIR    STATE_DIR "/staging"
#define SDK_ROOT       "/usr/sdk"
#define DOWNLOAD_CHUNK 65536
#define SHA256_SIZE    32

typedef struct {
    const catalog_entry* entry;
    const char*          root;            /* where the archive's own root lands, "" for / */
    char*                old_paths;       /* the path lines of an older release's record, NULL without one */
    tar_reader           tar;
    int                  file_fd;         /* the staged file being written, -1 between files */
    uint32_t             file_mode;
    char*                paths;           /* the record's path lines, in archive order */
    size_t               paths_length;
    size_t               paths_capacity;
    bool                 changes_drop_ins;
    char                 real[PATH_MAX];
    char                 staged[PATH_MAX];
} install_job;

static install_job g_job;
static uint8_t     g_download[DOWNLOAD_CHUNK];
static uint8_t     g_unpacked[ZSTD_BLOCKSIZE_MAX];
static char        g_url[STLXHTTP_URL_MAX + 1];

/* Build packages hold one directory named after the package, which belongs under /usr/sdk */
static bool is_build_package(const char* name) {
    const char* list = STLXPKG_BUILD_PACKAGES;
    size_t length = strlen(name);
    while (*list != '\0') {
        size_t word_length = strcspn(list, " ");
        if (word_length == length && strncmp(list, name, length) == 0) {
            return true;
        }

        list += word_length;
        list += strspn(list, " ");
    }

    return false;
}

static int add_path_line(install_job* job, const char* path, bool is_directory) {
    size_t length = strlen(path) + (is_directory ? 2 : 1);
    if (job->paths_length + length >= job->paths_capacity) {
        size_t capacity = 2 * (job->paths_capacity + length);
        char* grown = realloc(job->paths, capacity);
        if (!grown) {
            fprintf(stderr, "stlxpkg: out of memory\n");
            return -1;
        }

        job->paths = grown;
        job->paths_capacity = capacity;
    }

    snprintf(job->paths + job->paths_length, length + 1, "%s%s\n", path, is_directory ? "/" : "");
    job->paths_length += length;

    return 0;
}

static void report_conflict(const install_job* job, bool is_directory) {
    char line[PATH_MAX + 1];
    char owner[CATALOG_FILE_MAX];
    snprintf(line, sizeof(line), "%s%s", job->real, is_directory ? "/" : "");
    if (record_find_owner(line, owner, sizeof(owner))) {
        fprintf(stderr, "stlxpkg: %s cannot install %s, which belongs to %s\n", job->entry->name, job->real, owner);
        return;
    }

    fprintf(stderr, "stlxpkg: %s cannot install %s, which is already there and was not installed by stlxpkg\n",
            job->entry->name, job->real);
}

/* A path is free when missing, when a directory and so is the entry, as directories are shared, or when a file the
 * package's older release installed. Anything else there belongs to another package or to the image. */
static int check_path_free(const install_job* job, bool is_directory) {
    struct stat status;
    if (lstat(job->real, &status) != 0) {
        if (errno == ENOENT) {
            return 0;
        }

        fprintf(stderr, "stlxpkg: cannot check %s (errno %d)\n", job->real, errno);
        return -1;
    }

    bool exists_as_directory = S_ISDIR(status.st_mode);
    if (is_directory && exists_as_directory) {
        return 0;
    }

    if (!is_directory && !exists_as_directory && job->old_paths && record_paths_hold(job->old_paths, job->real)) {
        return 0;
    }

    report_conflict(job, exists_as_directory);

    return -1;
}

static int stage_entry(install_job* job, const tar_entry* entry) {
    if (make_parent_directories(job->staged) != 0) {
        return -1;
    }

    if (entry->type == TAR_DIRECTORY) {
        return make_directories(job->staged);
    }

    if (entry->type == TAR_SYMLINK) {
        return symlink(entry->target, job->staged);
    }

    job->file_fd = open(job->staged, O_WRONLY | O_CREAT | O_EXCL, 0600);
    job->file_mode = entry->mode;

    return job->file_fd < 0 ? -1 : 0;
}

static int on_entry(void* context, const tar_entry* entry) {
    install_job* job = context;
    bool is_directory = entry->type == TAR_DIRECTORY;
    int real_length = snprintf(job->real, sizeof(job->real), "%s/%s", job->root, entry->path);
    int staged_length = snprintf(job->staged, sizeof(job->staged), "%s%s", STAGING_DIR, job->real);
    if (real_length >= (int)sizeof(job->real) || staged_length >= (int)sizeof(job->staged)) {
        fprintf(stderr, "stlxpkg: %s holds a path longer than %d characters\n", job->entry->name, PATH_MAX);
        return -1;
    }

    if (check_path_free(job, is_directory) != 0 || add_path_line(job, job->real, is_directory) != 0) {
        return -1;
    }

    if (stage_entry(job, entry) != 0) {
        fprintf(stderr, "stlxpkg: cannot stage %s (errno %d)\n", job->real, errno);
        return -1;
    }

    if (desktop_reads(job->real)) {
        job->changes_drop_ins = true;
    }

    return 0;
}

static int on_data(void* context, const uint8_t* bytes, size_t length) {
    install_job* job = context;
    while (length > 0) {
        ssize_t count = write(job->file_fd, bytes, length);
        if (count <= 0) {
            fprintf(stderr, "stlxpkg: cannot stage %s (errno %d)\n", job->real, errno);
            return -1;
        }

        bytes += count;
        length -= (size_t)count;
    }

    return 0;
}

static int on_end_file(void* context) {
    install_job* job = context;
    bool finished = fchmod(job->file_fd, job->file_mode) == 0;
    finished = close(job->file_fd) == 0 && finished;
    job->file_fd = -1;
    if (!finished) {
        fprintf(stderr, "stlxpkg: cannot stage %s (errno %d)\n", job->real, errno);
        return -1;
    }

    return 0;
}

static void report_tar_error(const install_job* job, int rc) {
    const char* problem = "a malformed tar header";
    if (rc == TAR_ERR_TYPE) {
        problem = "an entry that is not a file, a directory or a symlink";
    } else if (rc == TAR_ERR_PATH) {
        problem = "a path that leaves its root";
    }

    if (rc != TAR_ERR_HANDLER) {
        fprintf(stderr, "stlxpkg: %s's archive holds %s\n", job->entry->name, problem);
    }
}

/* Decompresses one downloaded chunk into the tar reader. `frame_left` becomes 0 once zstd ended the frame and
 * checked its checksum. */
static int unpack_chunk(install_job* job, ZSTD_DCtx* zstd, size_t length, size_t* frame_left) {
    ZSTD_inBuffer input = { g_download, length, 0 };
    while (input.pos < input.size) {
        ZSTD_outBuffer output = { g_unpacked, sizeof(g_unpacked), 0 };
        *frame_left = ZSTD_decompressStream(zstd, &output, &input);
        if (ZSTD_isError(*frame_left)) {
            fprintf(stderr, "stlxpkg: %s's archive does not decompress: %s\n", job->entry->name,
                    ZSTD_getErrorName(*frame_left));
            return -1;
        }

        int rc = tar_feed(&job->tar, g_unpacked, output.pos);
        if (rc != TAR_OK) {
            report_tar_error(job, rc);
            return -1;
        }
    }

    return 0;
}

static int check_archive_end(const install_job* job, br_sha256_context* sha256, size_t frame_left) {
    uint8_t digest[SHA256_SIZE];
    char hex[2 * SHA256_SIZE + 1];
    br_sha256_out(sha256, digest);
    for (size_t i = 0; i < SHA256_SIZE; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }

    const char* problem = NULL;
    if (frame_left != 0) {
        problem = "ends inside its zstd frame";
    } else if (!tar_ended(&job->tar)) {
        problem = "ends before its tar end";
    } else if (strcmp(hex, job->entry->sha256) != 0) {
        problem = "does not match its SHA-256 in the release's SHA256SUMS";
    }

    if (problem) {
        fprintf(stderr, "stlxpkg: %s's archive %s\n", job->entry->name, problem);
        return -1;
    }

    return 0;
}

static int stream_archive(install_job* job, stlxhttp_request* request) {
    br_sha256_context sha256;
    br_sha256_init(&sha256);
    ZSTD_DCtx* zstd = ZSTD_createDCtx();
    if (!zstd) {
        fprintf(stderr, "stlxpkg: out of memory\n");
        return -1;
    }

    size_t frame_left = 1;
    int64_t count = stlxhttp_read(request, g_download, sizeof(g_download));
    while (count > 0) {
        br_sha256_update(&sha256, g_download, (size_t)count);
        if (unpack_chunk(job, zstd, (size_t)count, &frame_left) != 0) {
            break;
        }

        count = stlxhttp_read(request, g_download, sizeof(g_download));
    }

    ZSTD_freeDCtx(zstd);
    if (count < 0) {
        fprintf(stderr, "stlxpkg: %s\n", stlxhttp_error_message(request));
    }

    return count == 0 ? check_archive_end(job, &sha256, frame_left) : -1;
}

/* Moves each staged path into place in archive order, so every directory exists before what it holds */
static int commit_paths(install_job* job) {
    const char* line = job->paths;
    const char* end = job->paths + job->paths_length;
    while (line < end) {
        size_t length = strcspn(line, "\n");
        bool is_directory = line[length - 1] == '/';
        size_t path_length = is_directory ? length - 1 : length;
        memcpy(job->real, line, path_length);
        job->real[path_length] = '\0';
        snprintf(job->staged, sizeof(job->staged), "%s%s", STAGING_DIR, job->real);

        bool placed = is_directory ? make_directories(job->real) == 0 :
                                     make_parent_directories(job->real) == 0 && rename(job->staged, job->real) == 0;
        if (!placed) {
            fprintf(stderr, "stlxpkg: cannot move %s into place (errno %d)\n", job->real, errno);
            return -1;
        }

        line += length + 1;
    }

    return 0;
}

/* Streams, verifies and commits the archive, then deletes what only the older release had. Staging is deleted on
 * every path, since only directories remain in it once the commit moved the rest. */
static int install_archive(install_job* job, const catalog* catalog) {
    catalog_archive_url(catalog, job->entry, g_url, sizeof(g_url));
    stlxhttp_request* request = NULL;
    int rc = -1;
    if (stlxhttp_open(g_url, 0, &request) == STLXHTTP_OK) {
        rc = stream_archive(job, request);
    } else {
        fprintf(stderr, "stlxpkg: %s\n", stlxhttp_error_message(request));
    }

    stlxhttp_close(request);
    if (job->file_fd >= 0) {
        close(job->file_fd);
        job->file_fd = -1;
    }

    if (rc == 0) {
        rc = record_write(job->entry, job->paths, job->paths_length);
    }

    if (rc == 0) {
        rc = commit_paths(job);
    }

    if (rc == 0 && job->old_paths) {
        rc = remove_paths(job->old_paths, job->paths, &job->changes_drop_ins);
    }

    (void)remove_tree(STAGING_DIR);

    return rc;
}

int install_package(const catalog* catalog, const char* name) {
    const catalog_entry* entry = catalog_find(catalog, name);
    if (!entry) {
        fprintf(stderr, "stlxpkg: %s has no package %s for this machine\n", catalog->tag, name);
        return -1;
    }

    catalog_entry installed;
    char* old_paths = NULL;
    int found = record_read(name, &installed, &old_paths);
    if (found < 0) {
        return -1;
    }

    if (found == 1 && strcmp(installed.sha256, entry->sha256) == 0) {
        printf("stlxpkg: %s %s is installed\n", name, installed.version);
        free(old_paths);
        return 0;
    }

    if (remove_tree(STAGING_DIR) != 0) {
        fprintf(stderr, "stlxpkg: cannot delete %s (errno %d)\n", STAGING_DIR, errno);
        free(old_paths);
        return -1;
    }

    install_job* job = &g_job;
    free(job->paths);
    free(job->old_paths);
    memset(job, 0, sizeof(*job));

    job->entry = entry;
    job->root = is_build_package(name) ? SDK_ROOT : "";
    job->old_paths = old_paths;
    job->file_fd = -1;

    tar_handlers handlers = { on_entry, on_data, on_end_file, job };
    tar_init(&job->tar, &handlers);
    if (old_paths) {
        printf("stlxpkg: upgrading %s %s to %s from %s\n", name, installed.version, entry->version, catalog->tag);
    } else {
        printf("stlxpkg: downloading %s %s from %s\n", name, entry->version, catalog->tag);
    }

    if (install_archive(job, catalog) != 0) {
        return -1;
    }

    if (job->changes_drop_ins) {
        desktop_reload();
    }

    printf("stlxpkg: installed %s %s%s%s\n", name, entry->version, job->root[0] ? " under " : "", job->root);

    return 0;
}
