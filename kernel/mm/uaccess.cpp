#include "mm/uaccess.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "mm/mm.h"
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

    // Pre-fault any lazy pages in the validated range so that the kernel-mode
    // memcpy in copy_from_user/copy_to_user doesn't fault on a not-present PTE.
    uintptr_t end_page = end & ~(pmm::PAGE_SIZE - 1);
    for (uintptr_t page = start & ~(pmm::PAGE_SIZE - 1);
         page <= end_page;
         page += pmm::PAGE_SIZE) {
        if (paging::get_physical(page, mm_ctx->pt_root) != 0) {
            continue;
        }

        if (!handle_user_pf(mm_ctx, page, 0)) {
            return ERR_FAULT;
        }
    }

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
__PRIVILEGED_CODE int32_t copy_to_user_nonblock(
    void* udst,
    const void* ksrc,
    size_t len
) {
    if (!udst || !ksrc || len == 0) {
        return ERR_INVAL;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(udst);
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

    // Interrupt context cannot block on the address-space lock
    mm_context* mm_ctx = task->exec.mm_ctx;
    if (!sync::mutex_trylock(mm_ctx->lock)) {
        return ERR_RETRY;
    }

    uintptr_t cursor = start;
    while (cursor <= end) {
        vma* region = vma_find_locked(mm_ctx, cursor);
        if (!region || cursor < region->start || cursor >= region->end ||
            (region->prot & MM_PROT_WRITE) == 0) {
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

    // Fault lazy stack pages in under the held lock, nothing blocks. A present
    // entry in a writable region is writable, nothing maps copy-on-write.
    uintptr_t end_page = end & ~(pmm::PAGE_SIZE - 1);
    for (uintptr_t page = start & ~(pmm::PAGE_SIZE - 1);
         page <= end_page;
         page += pmm::PAGE_SIZE) {
        if (paging::get_physical(page, mm_ctx->pt_root) != 0) {
            continue;
        }

        if (!handle_user_pf_locked(mm_ctx, page, 0)) {
            sync::mutex_unlock(mm_ctx->lock);
            return ERR_FAULT;
        }
    }

    // Copying under the held lock keeps a concurrent unmap out of the range
    string::memcpy(udst, ksrc, len);
    sync::mutex_unlock(mm_ctx->lock);

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

    size_t i = 0;
    while (i < cap) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(usrc + i);
        size_t remaining_in_page = pmm::PAGE_SIZE - (addr & (pmm::PAGE_SIZE - 1));
        size_t remaining_total = cap - i;
        size_t chunk = remaining_total < remaining_in_page ? remaining_total : remaining_in_page;

        int32_t rc = validate_user_range(usrc + i, chunk, MM_PROT_READ);
        if (rc != OK) {
            return rc;
        }

        for (size_t j = 0; j < chunk; j++) {
            char c = usrc[i + j];
            kdst[i + j] = c;
            if (c == '\0') {
                return OK;
            }
        }

        i += chunk;
    }

    kdst[cap - 1] = '\0';

    return ERR_NAMETOOLONG;
}

} // namespace mm::uaccess
