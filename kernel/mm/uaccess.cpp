#include "mm/uaccess.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "mm/mm.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/mutex.h"

namespace {

// Text range of one user access routine and where it resumes after a fault
struct access_region {
    const char* begin;
    const char* end;
    const char* fixup;
};

} // namespace

extern "C" size_t stlx_user_copy(void* dst, const void* src, size_t len);
extern "C" char stlx_user_copy_begin[];
extern "C" char stlx_user_copy_end[];
extern "C" char stlx_user_copy_fixup[];

static const access_region g_access_regions[] = {
    { stlx_user_copy_begin, stlx_user_copy_end, stlx_user_copy_fixup },
};

namespace mm::uaccess {

// Ring 0 can reach kernel memory through a user pointer, so the bound is
// checked in software before the hardware sees the access
static bool user_range_ok(const void* ptr, size_t len) {
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t end = start + len - 1;
    return end >= start && end < USER_STACK_TOP;
}

static const access_region* find_access_region(uintptr_t pc) {
    for (const access_region& region : g_access_regions) {
        if (pc >= reinterpret_cast<uintptr_t>(region.begin) &&
            pc < reinterpret_cast<uintptr_t>(region.end)) {
            return &region;
        }
    }
    return nullptr;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool handle_kernel_fault(
    mm_context* mm_ctx,
    uintptr_t* pc,
    uintptr_t fault_addr,
    uint64_t pf_flags,
    bool can_sleep
) {
    const access_region* region = find_access_region(*pc);
    if (!region || fault_addr >= USER_STACK_TOP) {
        return false;
    }

    if (can_sleep && mm_ctx && handle_user_pf(mm_ctx, fault_addr, pf_flags)) {
        return true;
    }

    *pc = reinterpret_cast<uintptr_t>(region->fixup);
    return true;
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

    if (!user_range_ok(usrc, len)) {
        return ERR_FAULT;
    }

    sched::task* task = sched::current();
    if (!task || !task->exec.mm_ctx) {
        return ERR_NO_MMCTX;
    }

    return stlx_user_copy(kdst, usrc, len) == 0 ? OK : ERR_FAULT;
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

    if (!user_range_ok(udst, len)) {
        return ERR_FAULT;
    }

    sched::task* task = sched::current();
    if (!task || !task->exec.mm_ctx) {
        return ERR_NO_MMCTX;
    }

    return stlx_user_copy(udst, ksrc, len) == 0 ? OK : ERR_FAULT;
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

    if (!user_range_ok(udst, len)) {
        return ERR_FAULT;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(udst);
    uintptr_t end = start + len - 1;

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
    size_t left = stlx_user_copy(udst, ksrc, len);
    sync::mutex_unlock(mm_ctx->lock);

    return left == 0 ? OK : ERR_FAULT;
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

    sched::task* task = sched::current();
    if (!task || !task->exec.mm_ctx) {
        return ERR_NO_MMCTX;
    }

    // Page-sized chunks keep a string that ends before
    // an unmapped page from touching that page at all.
    size_t i = 0;
    while (i < cap) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(usrc + i);
        size_t remaining_in_page = pmm::PAGE_SIZE - (addr & (pmm::PAGE_SIZE - 1));
        size_t remaining_total = cap - i;
        size_t chunk = remaining_total < remaining_in_page ? remaining_total : remaining_in_page;

        if (!user_range_ok(usrc + i, chunk)) {
            return ERR_FAULT;
        }

        size_t copied = chunk - stlx_user_copy(kdst + i, usrc + i, chunk);
        for (size_t j = 0; j < copied; j++) {
            if (kdst[i + j] == '\0') {
                return OK;
            }
        }

        if (copied < chunk) {
            return ERR_FAULT;
        }

        i += chunk;
    }

    kdst[cap - 1] = '\0';

    return ERR_NAMETOOLONG;
}

} // namespace mm::uaccess
