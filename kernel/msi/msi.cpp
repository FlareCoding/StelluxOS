#include "msi/msi.h"
#include "arch/arch_msi.h"
#include "sync/spinlock.h"
#include "common/logging.h"

namespace msi {

struct handler_slot {
    handler_fn fn;
    void*      context;
};

__PRIVILEGED_BSS static handler_slot g_slots[MAX_VECTORS];
__PRIVILEGED_BSS static uint64_t    g_bitmap[(MAX_VECTORS + 63) / 64];
__PRIVILEGED_BSS static uint32_t    g_capacity;
__PRIVILEGED_BSS static bool        g_initialized;
__PRIVILEGED_DATA static sync::spinlock g_lock = sync::SPINLOCK_INIT;

static bool bitmap_test(uint32_t index) {
    return (g_bitmap[index / 64] & (1ULL << (index % 64))) != 0;
}

static void bitmap_set(uint32_t index) {
    g_bitmap[index / 64] |= (1ULL << (index % 64));
}

static void bitmap_clear(uint32_t index) {
    g_bitmap[index / 64] &= ~(1ULL << (index % 64));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    if (g_initialized) {
        return OK;
    }

    uint32_t cap = 0;
    int32_t rc = arch::msi_init(&cap);

    if (rc == ERR_NOT_SUPPORTED) {
        g_capacity = 0;
        g_initialized = true;
        log::info("msi: platform does not support MSI");
        return OK;
    }

    if (rc != OK) {
        log::error("msi: arch::msi_init failed (%d)", rc);
        return ERR_INIT;
    }

    if (cap > MAX_VECTORS) {
        cap = MAX_VECTORS;
    }

    g_capacity = cap;
    g_initialized = true;
    log::info("msi: initialized, %u vectors available", g_capacity);
    return OK;
}

uint32_t capacity() {
    return g_capacity;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t alloc(uint32_t count, uint32_t alignment,
                                uint32_t* out_base) {
    if (!g_initialized) {
        return ERR_NOT_READY;
    }
    if (count == 0 || count > g_capacity || out_base == nullptr) {
        return ERR_INVALID;
    }
    if (alignment == 0) {
        alignment = 1;
    }
    if ((alignment & (alignment - 1)) != 0) {
        return ERR_INVALID;
    }

    sync::irq_lock_guard guard(g_lock);

    for (uint32_t start = 0; start < g_capacity; start += alignment) {
        if (start + count > g_capacity) {
            break;
        }

        bool block_free = true;
        for (uint32_t j = 0; j < count; j++) {
            if (bitmap_test(start + j)) {
                block_free = false;
                break;
            }
        }

        if (block_free) {
            for (uint32_t j = 0; j < count; j++) {
                bitmap_set(start + j);
            }
            *out_base = start;
            return OK;
        }
    }

    return ERR_NO_VECTORS;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t free(uint32_t base, uint32_t count) {
    if (!g_initialized) {
        return ERR_NOT_READY;
    }
    if (count == 0 || base + count > g_capacity) {
        return ERR_INVALID;
    }

    sync::irq_lock_guard guard(g_lock);

    for (uint32_t i = base; i < base + count; i++) {
        bitmap_clear(i);
        g_slots[i].context = nullptr;
        __atomic_store_n(&g_slots[i].fn, static_cast<handler_fn>(nullptr),
                         __ATOMIC_RELEASE);
    }

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_handler(uint32_t vector,
                                      handler_fn fn, void* context) {
    if (vector >= g_capacity) {
        return ERR_INVALID;
    }

    sync::irq_lock_guard guard(g_lock);
    g_slots[vector].context = context;
    __atomic_store_n(&g_slots[vector].fn, fn, __ATOMIC_RELEASE);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void clear_handler(uint32_t vector) {
    if (vector >= g_capacity) {
        return;
    }

    sync::irq_lock_guard guard(g_lock);
    g_slots[vector].context = nullptr;
    __atomic_store_n(&g_slots[vector].fn, static_cast<handler_fn>(nullptr),
                     __ATOMIC_RELEASE);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dispatch(uint32_t vector) {
    if (vector >= g_capacity) {
        return;
    }
    handler_fn fn = __atomic_load_n(&g_slots[vector].fn, __ATOMIC_ACQUIRE);
    if (fn) {
        fn(vector, g_slots[vector].context);
    }
}

} // namespace msi
