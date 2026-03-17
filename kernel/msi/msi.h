#ifndef STELLUX_MSI_MSI_H
#define STELLUX_MSI_MSI_H

#include "common/types.h"

namespace msi {

struct message {
    uint64_t address;
    uint32_t data;
};

constexpr int32_t OK              =  0;
constexpr int32_t ERR_INIT        = -1;
constexpr int32_t ERR_NO_VECTORS  = -2;
constexpr int32_t ERR_INVALID     = -3;
constexpr int32_t ERR_NOT_READY   = -4;
constexpr int32_t ERR_NOT_SUPPORTED = -5;

constexpr uint32_t MAX_VECTORS = 128;

using handler_fn = void (*)(uint32_t vector, void* context);

/**
 * Initialize the MSI subsystem. Calls arch::msi_init() to discover
 * platform capacity. Must be called after irq::init() and pci::init().
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * Number of MSI vectors the platform supports. Valid after init().
 */
uint32_t capacity();

/**
 * Allocate `count` contiguous vectors with `alignment` constraint.
 * alignment=1: no alignment constraint (MSI-X).
 * alignment=count (power of two): naturally aligned (MSI).
 * Returns base index in *out_base. Range is [*out_base, *out_base + count).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t alloc(uint32_t count, uint32_t alignment,
                                uint32_t* out_base);

/**
 * Release vectors previously returned by alloc().
 * Clears any registered handlers for these vectors.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t free(uint32_t base, uint32_t count);

/**
 * Register a handler for an allocated vector. Uses release semantics
 * so dispatch (acquire) sees both fn and context consistently.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_handler(uint32_t vector,
                                      handler_fn fn, void* context);

/**
 * Remove the handler for a vector. Safe to call if none registered.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void clear_handler(uint32_t vector);

/**
 * Dispatch an MSI interrupt. Called by the arch trap handler with
 * a 0-based pool index. If no handler is registered, silently dropped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dispatch(uint32_t vector);

} // namespace msi

#endif // STELLUX_MSI_MSI_H
