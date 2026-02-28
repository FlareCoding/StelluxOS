#include "syscall/handlers/sys_mmap.h"

#include "mm/vma.h"
#include "sched/sched.h"
#include "sched/task.h"

namespace {

constexpr uint64_t LINUX_PROT_READ  = 0x1;
constexpr uint64_t LINUX_PROT_WRITE = 0x2;
constexpr uint64_t LINUX_PROT_EXEC  = 0x4;
constexpr uint64_t LINUX_PROT_MASK  = LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC;

constexpr uint64_t LINUX_MAP_PRIVATE         = 0x00000002;
constexpr uint64_t LINUX_MAP_FIXED           = 0x00000010;
constexpr uint64_t LINUX_MAP_ANONYMOUS       = 0x00000020;
constexpr uint64_t LINUX_MAP_STACK           = 0x00020000;
constexpr uint64_t LINUX_MAP_FIXED_NOREPLACE = 0x00100000;
constexpr uint64_t LINUX_MAP_ALLOWED_MASK =
    LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
    LINUX_MAP_STACK | LINUX_MAP_FIXED_NOREPLACE;

inline uint32_t linux_prot_to_mm(uint64_t prot) {
    uint32_t mm_prot = 0;
    if (prot & LINUX_PROT_READ) mm_prot |= mm::MM_PROT_READ;
    if (prot & LINUX_PROT_WRITE) mm_prot |= mm::MM_PROT_WRITE;
    if (prot & LINUX_PROT_EXEC) mm_prot |= mm::MM_PROT_EXEC;
    return mm_prot;
}

inline uint32_t linux_map_to_mm(uint64_t flags) {
    uint32_t mm_flags = 0;
    if (flags & LINUX_MAP_PRIVATE) mm_flags |= mm::MM_MAP_PRIVATE;
    if (flags & LINUX_MAP_FIXED) mm_flags |= mm::MM_MAP_FIXED;
    if (flags & LINUX_MAP_ANONYMOUS) mm_flags |= mm::MM_MAP_ANONYMOUS;
    if (flags & LINUX_MAP_STACK) mm_flags |= mm::MM_MAP_STACK;
    if (flags & LINUX_MAP_FIXED_NOREPLACE) mm_flags |= mm::MM_MAP_FIXED_NOREPLACE;
    return mm_flags;
}

inline int64_t mm_status_to_errno(int32_t status) {
    switch (status) {
        case mm::MM_CTX_OK:
            return 0;
        case mm::MM_CTX_ERR_EXISTS:
            return syscall::EEXIST;
        case mm::MM_CTX_ERR_INVALID_ARG:
            return syscall::EINVAL;
        case mm::MM_CTX_ERR_NOT_MAPPED:
            return syscall::ENOMEM;
        case mm::MM_CTX_ERR_NO_MEM:
        case mm::MM_CTX_ERR_NO_VIRT:
        case mm::MM_CTX_ERR_MAP_FAILED:
        default:
            return syscall::ENOMEM;
    }
}

inline bool is_page_aligned(uint64_t value) {
    return (value & (pmm::PAGE_SIZE - 1)) == 0;
}

} // namespace

DEFINE_SYSCALL6(mmap, addr, length, prot, flags, fd, offset) {
    if (length == 0) {
        return syscall::EINVAL;
    }
    if ((prot & ~LINUX_PROT_MASK) != 0) {
        return syscall::EINVAL;
    }
    if ((flags & ~LINUX_MAP_ALLOWED_MASK) != 0) {
        return syscall::EINVAL;
    }
    if (!(flags & LINUX_MAP_PRIVATE)) {
        return syscall::EINVAL;
    }
    if (!(flags & LINUX_MAP_ANONYMOUS)) {
        return syscall::EINVAL;
    }
    if (static_cast<int64_t>(fd) != -1) {
        return syscall::EINVAL;
    }
    if (offset != 0) {
        return syscall::EINVAL;
    }

    bool fixed = (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE)) != 0;
    if (fixed && !is_page_aligned(addr)) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task || !task->exec.mm_ctx) {
        return syscall::ENOMEM;
    }

    uintptr_t mapped_addr = 0;
    int32_t rc = mm::mm_context_map_anonymous(
        task->exec.mm_ctx,
        static_cast<uintptr_t>(addr),
        static_cast<size_t>(length),
        linux_prot_to_mm(prot),
        linux_map_to_mm(flags),
        &mapped_addr
    );
    if (rc != mm::MM_CTX_OK) {
        return mm_status_to_errno(rc);
    }

    return static_cast<int64_t>(mapped_addr);
}

DEFINE_SYSCALL2(munmap, addr, length) {
    if (!is_page_aligned(addr) || length == 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task || !task->exec.mm_ctx) {
        return syscall::ENOMEM;
    }

    int32_t rc = mm::mm_context_unmap(
        task->exec.mm_ctx,
        static_cast<uintptr_t>(addr),
        static_cast<size_t>(length)
    );
    if (rc != mm::MM_CTX_OK) {
        return mm_status_to_errno(rc);
    }
    return 0;
}

DEFINE_SYSCALL3(mprotect, addr, length, prot) {
    if (!is_page_aligned(addr) || length == 0) {
        return syscall::EINVAL;
    }
    if ((prot & ~LINUX_PROT_MASK) != 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task || !task->exec.mm_ctx) {
        return syscall::ENOMEM;
    }

    int32_t rc = mm::mm_context_mprotect(
        task->exec.mm_ctx,
        static_cast<uintptr_t>(addr),
        static_cast<size_t>(length),
        linux_prot_to_mm(prot)
    );
    if (rc != mm::MM_CTX_OK) {
        return mm_status_to_errno(rc);
    }
    return 0;
}
