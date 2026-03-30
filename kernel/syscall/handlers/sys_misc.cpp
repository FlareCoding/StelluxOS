#include "syscall/handlers/sys_misc.h"
#include "mm/uaccess.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "fs/node.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"

/**
 * access(pathname, mode) — Check file accessibility.
 *
 * Simplified implementation: checks if the file exists by doing a lookup.
 * Always grants read/write/execute access if the file exists (no real
 * permission model yet).
 */
DEFINE_SYSCALL2(access, u_pathname, mode) {
    (void)mode;

    char kpath[fs::PATH_MAX];
    int32_t rc = mm::uaccess::copy_cstr_from_user(
        kpath, sizeof(kpath), reinterpret_cast<const char*>(u_pathname));
    if (rc != mm::uaccess::OK) return syscall::EFAULT;
    if (kpath[0] == '\0') return syscall::ENOENT;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    // Resolve path relative to cwd
    fs::node* base = task->cwd;
    fs::node* target = nullptr;
    int32_t fs_rc;
    if (base) {
        fs_rc = fs::lookup_at(base, kpath, &target);
    } else {
        fs_rc = fs::lookup(kpath, &target);
    }

    if (fs_rc != fs::OK) {
        return syscall::ENOENT;
    }

    // File exists — release ref and return success
    if (target->release()) {
        fs::node::ref_destroy(target);
    }
    return 0;
}

/**
 * uname(buf) — Get system identification.
 *
 * Returns Stellux system information in a Linux-compatible utsname struct.
 */
DEFINE_SYSCALL1(uname, u_buf) {
    if (u_buf == 0) return syscall::EFAULT;

    // Linux struct utsname: 5 fields of 65 bytes each
    struct utsname {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
    };

    utsname kbuf;
    string::memset(&kbuf, 0, sizeof(kbuf));
    string::memcpy(kbuf.sysname, "Stellux", 8);
    string::memcpy(kbuf.nodename, "stellux", 8);
    string::memcpy(kbuf.release, "3.0.0", 6);
    string::memcpy(kbuf.version, "Stellux 3.0 Prototype", 22);
#if defined(__x86_64__)
    string::memcpy(kbuf.machine, "x86_64", 7);
#elif defined(__aarch64__)
    string::memcpy(kbuf.machine, "aarch64", 8);
#endif

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_buf), &kbuf, sizeof(kbuf));
    return (rc == mm::uaccess::OK) ? 0 : syscall::EFAULT;
}

/**
 * rename(oldpath, newpath) — Rename a file.
 *
 * Currently not implemented in Stellux's filesystem.
 * Returns ENOSYS so Vim falls back to write-in-place strategy.
 */
DEFINE_SYSCALL2(rename, u_oldpath, u_newpath) {
    (void)u_oldpath;
    (void)u_newpath;
    return syscall::ENOSYS;
}
