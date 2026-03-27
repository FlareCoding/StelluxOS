#include "irq/irq.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace irq {

namespace {

struct irq_entry {
    irq_handler_fn fn;
    void*          context;
};

__PRIVILEGED_DATA static irq_entry g_irq_table[MAX_IRQ] = {};
__PRIVILEGED_DATA static sync::spinlock g_irq_table_lock = sync::SPINLOCK_INIT;

} // anonymous namespace

__PRIVILEGED_CODE int32_t register_handler(uint32_t irq, irq_handler_fn fn,
                                           void* context) {
    if (irq >= MAX_IRQ || !fn) {
        return ERR_INVAL;
    }

    int32_t result = OK;
    sync::irq_state flags = sync::spin_lock_irqsave(g_irq_table_lock);

    if (g_irq_table[irq].fn != nullptr) {
        result = ERR_BUSY;
    } else {
        g_irq_table[irq].context = context;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_irq_table[irq].fn = fn;
    }

    sync::spin_unlock_irqrestore(g_irq_table_lock, flags);
    return result;
}

__PRIVILEGED_CODE void unregister_handler(uint32_t irq) {
    if (irq >= MAX_IRQ) {
        return;
    }

    sync::irq_state flags = sync::spin_lock_irqsave(g_irq_table_lock);
    g_irq_table[irq].fn = nullptr;
    g_irq_table[irq].context = nullptr;
    sync::spin_unlock_irqrestore(g_irq_table_lock, flags);
}

__PRIVILEGED_CODE bool dispatch(uint32_t irq) {
    if (irq >= MAX_IRQ) {
        return false;
    }

    // Acquire fence ensures we see the context written before fn.
    irq_handler_fn fn = g_irq_table[irq].fn;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if (fn) {
        fn(irq, g_irq_table[irq].context);
        return true;
    }

    return false;
}

} // namespace irq
