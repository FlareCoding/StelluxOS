#include "syscall/handlers/sys_uname.h"
#include "mm/uaccess.h"
#include "common/string.h"

namespace {

constexpr size_t UTS_FIELD_LEN = 65;

struct new_utsname {
    char sysname[UTS_FIELD_LEN];
    char nodename[UTS_FIELD_LEN];
    char release[UTS_FIELD_LEN];
    char version[UTS_FIELD_LEN];
    char machine[UTS_FIELD_LEN];
    char domainname[UTS_FIELD_LEN];
};

__PRIVILEGED_CODE static void fill_field(char* dst, const char* src) {
    size_t len = string::strnlen(src, UTS_FIELD_LEN - 1);
    string::memcpy(dst, src, len);
    string::memset(dst + len, 0, UTS_FIELD_LEN - len);
}

} // anonymous namespace

/**
 * @note Privilege: **required**
 */
DEFINE_SYSCALL1(uname, u_buf) {
    if (u_buf == 0) {
        return syscall::EFAULT;
    }

    new_utsname kbuf;

    fill_field(kbuf.sysname,    "Stellux");
    fill_field(kbuf.nodename,   "stellux");
    fill_field(kbuf.release,    "3.0.0");
    fill_field(kbuf.version,    "Stellux 3.0.0");
#if defined(__x86_64__)
    fill_field(kbuf.machine,    "x86_64");
#elif defined(__aarch64__)
    fill_field(kbuf.machine,    "aarch64");
#endif
    fill_field(kbuf.domainname, "(none)");

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_buf), &kbuf, sizeof(kbuf));
    if (rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}
