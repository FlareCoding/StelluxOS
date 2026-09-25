#include "syscall/handlers/sys_prctl.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "common/string.h"

constexpr int32_t PR_SET_NAME = 15;
constexpr int32_t PR_GET_NAME = 16;

// Thread names cross the ABI in 16-byte buffers, terminator included
constexpr size_t THREAD_NAME_BYTES = 16;

// Names the calling thread. The ABI reads at most 15 bytes and never past a terminator,
// so the copy goes a byte at a time and an unterminated name is cut to 15 characters.
static int64_t set_thread_name(uint64_t u_name) {
    char name[THREAD_NAME_BYTES] = {};
    for (size_t i = 0; i < THREAD_NAME_BYTES - 1; i++) {
        if (mm::uaccess::copy_from_user(&name[i], reinterpret_cast<const void*>(u_name + i), 1) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }

        if (name[i] == '\0') {
            break;
        }
    }

    sched::task* self = sched::current();
    size_t len = string::strnlen(name, THREAD_NAME_BYTES - 1);
    string::memcpy(self->name, name, len);
    self->name[len] = '\0';
    return 0;
}

// Copies the calling thread's name out, cut to 15 characters and zero padded
static int64_t get_thread_name(uint64_t u_name) {
    char name[THREAD_NAME_BYTES] = {};
    sched::task* self = sched::current();
    string::memcpy(name, self->name, string::strnlen(self->name, THREAD_NAME_BYTES - 1));

    if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_name), name, sizeof(name)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}

DEFINE_SYSCALL2(prctl, u_option, u_arg2) {
    switch (static_cast<int32_t>(u_option)) {
    case PR_SET_NAME:
        return set_thread_name(u_arg2);
    case PR_GET_NAME:
        return get_thread_name(u_arg2);
    default:
        return syscall::EINVAL;
    }
}
