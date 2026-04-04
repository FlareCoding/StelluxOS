#include "syscall/handlers/sys_mmap.h"

#include "mm/vma.h"
#include "mm/shmem.h"
#include "resource/resource.h"
#include "resource/providers/shmem_resource_provider.h"
#include "sched/sched.h"
#include "sched/task.h"

namespace {

constexpr uint64_t LINUX_PROT_READ  = 0x1;
constexpr uint64_t LINUX_PROT_WRITE = 0x2;
constexpr uint64_t LINUX_PROT_EXEC  = 0x4;
constexpr uint64_t LINUX_PROT_MASK  = LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC;

constexpr uint64_t LINUX_MAP_SHARED          = 0x00000001;
constexpr uint64_t LINUX_MAP_PRIVATE         = 0x00000002;
constexpr uint64_t LINUX_MAP_FIXED           = 0x00000010;
constexpr uint64_t LINUX_MAP_ANONYMOUS       = 0x00000020;
constexpr uint64_t LINUX_MAP_POPULATE        = 0x00008000;
constexpr uint64_t LINUX_MAP_STACK           = 0x00020000;
constexpr uint64_t LINUX_MAP_FIXED_NOREPLACE = 0x00100000;
constexpr uint64_t LINUX_MAP_ALLOWED_MASK =
    LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | LINUX_MAP_FIXED |
    LINUX_MAP_ANONYMOUS | LINUX_MAP_POPULATE | LINUX_MAP_STACK |
    LINUX_MAP_FIXED_NOREPLACE;

inline uint32_t linux_prot_to_mm(uint64_t prot) {
    uint32_t mm_prot = 0;
    if (prot & LINUX_PROT_READ) mm_prot |= mm::MM_PROT_READ;
    if (prot & LINUX_PROT_WRITE) mm_prot |= mm::MM_PROT_WRITE;
    if (prot & LINUX_PROT_EXEC) mm_prot |= mm::MM_PROT_EXEC;
    return mm_prot;
}

inline uint32_t linux_map_to_mm(uint64_t flags) {
    uint32_t mm_flags = 0;
    if (flags & LINUX_MAP_SHARED) mm_flags |= mm::MM_MAP_SHARED;
    if (flags & LINUX_MAP_PRIVATE) mm_flags |= mm::MM_MAP_PRIVATE;
    if (flags & LINUX_MAP_FIXED) mm_flags |= mm::MM_MAP_FIXED;
    if (flags & LINUX_MAP_ANONYMOUS) mm_flags |= mm::MM_MAP_ANONYMOUS;
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

    bool has_shared = (flags & LINUX_MAP_SHARED) != 0;
    bool has_private = (flags & LINUX_MAP_PRIVATE) != 0;
    bool has_anon = (flags & LINUX_MAP_ANONYMOUS) != 0;
    int64_t fd_val = static_cast<int64_t>(fd);

    if (has_shared == has_private) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task || !task->exec.mm_ctx) {
        return syscall::ENOMEM;
    }

    if (fd_val != -1 && !has_anon) {
        if (!has_shared) {
            return syscall::EINVAL;
        }
        if (!is_page_aligned(offset)) {
            return syscall::EINVAL;
        }
        if (offset + length < length) {
            return syscall::EINVAL;
        }

        uint32_t required_rights = 0;
        if (prot & LINUX_PROT_READ) required_rights |= resource::RIGHT_READ;
        if (prot & LINUX_PROT_WRITE) required_rights |= resource::RIGHT_WRITE;

        resource::resource_object* obj = nullptr;
        int32_t rc = resource::get_handle_object(
            &task->handles, static_cast<int32_t>(fd_val),
            required_rights, &obj);
        if (rc != resource::HANDLE_OK) {
            return (rc == resource::HANDLE_ERR_ACCESS) ?
                   syscall::EACCES : syscall::EBADF;
        }

        uintptr_t mapped_addr = 0;

        if (obj->type == resource::resource_type::SHMEM) {
            mm::shmem* backing = resource::shmem_resource_provider::get_shmem_backing(obj);
            if (!backing) {
                resource::resource_release(obj);
                return syscall::EINVAL;
            }

            int32_t map_rc = mm::mm_context_map_shared(
                task->exec.mm_ctx,
                backing,
                static_cast<uint64_t>(offset),
                static_cast<size_t>(length),
                linux_prot_to_mm(prot),
                linux_map_to_mm(flags),
                static_cast<uintptr_t>(addr),
                &mapped_addr
            );

            resource::resource_release(obj);

            if (map_rc != mm::MM_CTX_OK) {
                return mm_status_to_errno(map_rc);
            }
            return static_cast<int64_t>(mapped_addr);
        } else if (obj->ops && obj->ops->mmap) {
            int32_t map_rc = obj->ops->mmap(
                obj, task->exec.mm_ctx,
                static_cast<uintptr_t>(addr),
                static_cast<size_t>(length),
                linux_prot_to_mm(prot),
                linux_map_to_mm(flags),
                static_cast<uint64_t>(offset),
                &mapped_addr);

            resource::resource_release(obj);

            if (map_rc != 0) {
                return mm_status_to_errno(map_rc);
            }
            return static_cast<int64_t>(mapped_addr);
        } else {
            resource::resource_release(obj);
            return syscall::EINVAL;
        }
    }

    if (!has_private) {
        return syscall::EINVAL;
    }
    if (!has_anon) {
        return syscall::EINVAL;
    }
    if (fd_val != -1) {
        return syscall::EINVAL;
    }
    if (offset != 0) {
        return syscall::EINVAL;
    }

    bool fixed = (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE)) != 0;
    if (fixed && !is_page_aligned(addr)) {
        return syscall::EINVAL;
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

DEFINE_SYSCALL1(brk, addr) {
    (void)addr;
    // Minimal brk: always return 0 (break at address 0).
    // musl interprets this as "brk not available" and falls back to mmap.
    return 0;
}

// No-op: performance hints. Safe to ignore.
DEFINE_SYSCALL3(madvise, addr, length, advice) {
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

DEFINE_SYSCALL4(fadvise64, fd, offset, len, advice) {
    (void)fd;
    (void)offset;
    (void)len;
    (void)advice;
    return 0;
}
