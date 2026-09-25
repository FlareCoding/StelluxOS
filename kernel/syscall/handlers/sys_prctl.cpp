#include "syscall/handlers/sys_prctl.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/spinlock.h"
#include "mm/uaccess.h"
#include "common/string.h"

constexpr int32_t PR_GET_DUMPABLE     = 3;
constexpr int32_t PR_SET_DUMPABLE     = 4;
constexpr int32_t PR_SET_NAME         = 15;
constexpr int32_t PR_GET_NAME         = 16;
constexpr int32_t PR_SET_TIMERSLACK   = 29;
constexpr int32_t PR_GET_TIMERSLACK   = 30;
constexpr int32_t PR_SET_NO_NEW_PRIVS = 38;
constexpr int32_t PR_GET_NO_NEW_PRIVS = 39;

// Thread names cross the ABI in 16-byte buffers, terminator included
constexpr size_t THREAD_NAME_BYTES = 16;

// The only values PR_SET_DUMPABLE and PR_SET_NO_NEW_PRIVS accept
constexpr uint64_t DUMPABLE_OFF    = 0;
constexpr uint64_t DUMPABLE_ON     = 1;
constexpr uint64_t NO_NEW_PRIVS_ON = 1;

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

// The dumpable flag belongs to the process, so a caller without one is refused
static int64_t set_dumpable(uint64_t value) {
    sched::thread_group* group = sched::current()->group;
    if (!group) {
        return syscall::ESRCH;
    }

    if (value != DUMPABLE_OFF && value != DUMPABLE_ON) {
        return syscall::EINVAL;
    }

    sync::irq_lock_guard guard(group->lock);
    group->dumpable = value == DUMPABLE_ON;
    return 0;
}

static int64_t get_dumpable() {
    sched::thread_group* group = sched::current()->group;
    if (!group) {
        return syscall::ESRCH;
    }

    sync::irq_lock_guard guard(group->lock);
    return static_cast<int64_t>(group->dumpable ? DUMPABLE_ON : DUMPABLE_OFF);
}

DEFINE_SYSCALL5(prctl, u_option, u_arg2, u_arg3, u_arg4, u_arg5) {
    sched::task* self = sched::current();
    bool extra_args = u_arg3 || u_arg4 || u_arg5;

    switch (static_cast<int32_t>(u_option)) {
    case PR_GET_DUMPABLE:
        return get_dumpable();
    case PR_SET_DUMPABLE:
        return set_dumpable(u_arg2);
    case PR_SET_NAME:
        return set_thread_name(u_arg2);
    case PR_GET_NAME:
        return get_thread_name(u_arg2);
    case PR_SET_TIMERSLACK:
        // Zero restores the slack the thread started with
        self->timer_slack_ns = u_arg2 ? u_arg2 : self->default_timer_slack_ns;
        return 0;
    case PR_GET_TIMERSLACK:
        return static_cast<int64_t>(self->timer_slack_ns);
    case PR_SET_NO_NEW_PRIVS:
        if (u_arg2 != NO_NEW_PRIVS_ON || extra_args) {
            return syscall::EINVAL;
        }

        self->no_new_privs = true;
        return 0;
    case PR_GET_NO_NEW_PRIVS:
        if (u_arg2 || extra_args) {
            return syscall::EINVAL;
        }

        return self->no_new_privs ? 1 : 0;
    default:
        return syscall::EINVAL;
    }
}
