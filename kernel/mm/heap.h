#ifndef STELLUX_MM_HEAP_H
#define STELLUX_MM_HEAP_H

#include "common/types.h"

namespace heap {

constexpr int32_t OK            = 0;
constexpr int32_t ERR_NO_MEMORY = -1;
constexpr int32_t ERR_BAD_PTR   = -2;
constexpr int32_t ERR_CORRUPT   = -3;

/**
 * @brief Initialize both heaps. Call after vmm::init().
 * @return OK on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Allocate from the privileged heap (PAGE_KERNEL_RW).
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE void* kalloc(size_t size);

/**
 * @brief Allocate zeroed memory from the privileged heap.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE void* kzalloc(size_t size);

/**
 * @brief Free to the privileged heap. Panics on heap mismatch.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t kfree(void* ptr);

/**
 * @brief Allocate from the unprivileged heap (PAGE_USER_RW).
 * Auto-elevates if called from unprivileged kernel context.
 */
[[nodiscard]] void* ualloc(size_t size);

/**
 * @brief Allocate zeroed memory from the unprivileged heap.
 * Auto-elevates if called from unprivileged kernel context.
 */
[[nodiscard]] void* uzalloc(size_t size);

/**
 * @brief Free to the unprivileged heap. Panics on heap mismatch.
 * Auto-elevates if called from unprivileged kernel context.
 */
int32_t ufree(void* ptr);

/**
 * @brief Allocate and construct a T from the privileged heap.
 * @note Privilege: **required**
 */
template<typename T, typename... Args>
[[nodiscard]] __PRIVILEGED_CODE T* kalloc_new(Args&&... args) {
    void* p = kzalloc(sizeof(T));
    return p ? new (p) T(static_cast<Args&&>(args)...) : nullptr;
}

/**
 * @brief Destroy and free a T to the privileged heap.
 * @note Privilege: **required**
 */
template<typename T>
__PRIVILEGED_CODE void kfree_delete(T* p) {
    if (p) { p->~T(); kfree(p); }
}

/**
 * @brief Allocate and construct a T from the unprivileged heap.
 * Auto-elevates if called from unprivileged kernel context.
 */
template<typename T, typename... Args>
[[nodiscard]] T* ualloc_new(Args&&... args) {
    void* p = uzalloc(sizeof(T));
    return p ? new (p) T(static_cast<Args&&>(args)...) : nullptr;
}

/**
 * @brief Destroy and free a T to the unprivileged heap.
 * Auto-elevates if called from unprivileged kernel context.
 */
template<typename T>
void ufree_delete(T* p) {
    if (p) { p->~T(); ufree(p); }
}

/**
 * @brief Dump heap statistics to serial.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_stats();

} // namespace heap

#endif // STELLUX_MM_HEAP_H
