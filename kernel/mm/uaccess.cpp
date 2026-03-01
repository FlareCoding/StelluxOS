#include "mm/uaccess.h"
#include "mm/vma.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/mutex.h"
#include "common/string.h"

namespace mm::uaccess {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t validate_user_range(
    const void* user_ptr,
    size_t len,
    uint32_t required_prot
) {
    if (!user_ptr || len == 0) {
        return ERR_INVAL;
    }
    if ((required_prot & ~MM_PROT_MASK) != 0 || required_prot == 0) {
        return ERR_INVAL;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(user_ptr);
    uintptr_t end = start + len - 1;
    if (end < start) {
        return ERR_INVAL;
    }
    if (end >= USER_STACK_TOP) {
        return ERR_FAULT;
    }

    sched::task* task = sched::current();
    if (!task || !task->exec.mm_ctx) {
        return ERR_NO_MMCTX;
    }

    mm_context* mm_ctx = task->exec.mm_ctx;
    sync::mutex_lock(mm_ctx->lock);

    uintptr_t cursor = start;
    while (cursor <= end) {
        vma* region = vma_find_locked(mm_ctx, cursor);
        if (!region || cursor < region->start || cursor >= region->end) {
            sync::mutex_unlock(mm_ctx->lock);
            return ERR_FAULT;
        }
        if ((region->prot & required_prot) != required_prot) {
            sync::mutex_unlock(mm_ctx->lock);
            return ERR_FAULT;
        }

        uintptr_t next = region->end;
        if (next == 0 || next <= cursor) {
            sync::mutex_unlock(mm_ctx->lock);
            return ERR_FAULT;
        }
        if (next > end) {
            break;
        }
        cursor = next;
    }

    sync::mutex_unlock(mm_ctx->lock);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_from_user(
    void* kdst,
    const void* usrc,
    size_t len
) {
    if (!kdst || !usrc) {
        return ERR_INVAL;
    }
    if (len == 0) {
        return OK;
    }

    int32_t rc = validate_user_range(usrc, len, MM_PROT_READ);
    if (rc != OK) {
        return rc;
    }

    string::memcpy(kdst, usrc, len);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_to_user(
    void* udst,
    const void* ksrc,
    size_t len
) {
    if (!udst || !ksrc) {
        return ERR_INVAL;
    }
    if (len == 0) {
        return OK;
    }

    int32_t rc = validate_user_range(udst, len, MM_PROT_WRITE);
    if (rc != OK) {
        return rc;
    }

    string::memcpy(udst, ksrc, len);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_cstr_from_user(
    char* kdst,
    size_t cap,
    const char* usrc
) {
    if (!kdst || !usrc || cap == 0) {
        return ERR_INVAL;
    }

    for (size_t i = 0; i < cap; i++) {
        int32_t rc = validate_user_range(usrc + i, 1, MM_PROT_READ);
        if (rc != OK) {
            return rc;
        }

        char c = usrc[i];
        kdst[i] = c;
        if (c == '\0') {
            return OK;
        }
    }

    kdst[cap - 1] = '\0';
    return ERR_NAMETOOLONG;
}

} // namespace mm::uaccess
