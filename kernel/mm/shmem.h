#ifndef STELLUX_MM_SHMEM_H
#define STELLUX_MM_SHMEM_H

#include "common/types.h"
#include "mm/pmm_types.h"
#include "rc/ref_counted.h"
#include "sync/mutex.h"

namespace mm {

constexpr int32_t SHMEM_OK             = 0;
constexpr int32_t SHMEM_ERR_INVAL      = -1;
constexpr int32_t SHMEM_ERR_NO_MEM     = -2;

/**
 * Ref-counted shared memory backing object.
 *
 * Holds an array of physical pages that can be mapped into multiple
 * mm_contexts simultaneously. Pages are freed only in ref_destroy
 * when the last reference is released.
 *
 * Lock order: when holding both mm_ctx->lock and shmem->lock,
 * always acquire mm_ctx->lock first.
 */
struct shmem final : rc::ref_counted<shmem> {
    pmm::phys_addr_t*  m_pages;
    size_t             m_page_count;
    size_t             m_capacity;
    size_t             m_size;
    sync::mutex        lock;

    static void ref_destroy(shmem* self);
};

/**
 * @brief Create a new shmem with the given initial size.
 * Pages are allocated and zeroed. Size 0 is valid (no pages allocated).
 * @return New shmem on success, nullptr on failure.
 */
[[nodiscard]] shmem* shmem_create(size_t initial_size);

/**
 * @brief Resize the shmem backing.
 * Grow: allocate and zero new pages.
 * Shrink: update m_size and m_page_count only; do NOT free tail pages
 * (they may still be mapped). Tail pages are freed in ref_destroy.
 * Caller must hold s->lock.
 */
int32_t shmem_resize_locked(shmem* s, size_t new_size);

/**
 * @brief Get the physical address of a page in the shmem.
 * Returns 0 if page_index >= m_page_count (hole).
 * Caller must hold s->lock.
 */
[[nodiscard]] pmm::phys_addr_t shmem_get_page_locked(
    shmem* s, size_t page_index);

/**
 * @brief Read from shmem at the given byte offset.
 * @return Number of bytes read, or negative error.
 */
ssize_t shmem_read(shmem* s, size_t offset, void* dst, size_t count);

/**
 * @brief Write to shmem at the given byte offset.
 * @return Number of bytes written, or negative error.
 */
ssize_t shmem_write(shmem* s, size_t offset, const void* src, size_t count);

} // namespace mm

#endif // STELLUX_MM_SHMEM_H
