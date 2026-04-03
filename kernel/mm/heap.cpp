#include "mm/heap.h"
#include "mm/heap_internal.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"
#include "common/string.h"

namespace heap {

__PRIVILEGED_DATA static heap_state g_priv_heap = {};
__PRIVILEGED_DATA static heap_state g_unpriv_heap = {};

static uint8_t size_to_class(size_t size) {
    for (uint8_t i = 0; i < CLASS_COUNT; i++) {
        if (size <= CLASS_SIZES[i]) return i;
    }
    return 0xFF;
}

// Validate that a freelist pointer is within the valid object region of its slab page.
__PRIVILEGED_CODE static bool validate_freelist_ptr(
    void* ptr, uintptr_t page_base, uint16_t obj_size, uint16_t objs_per_slab
) {
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    if ((p & ~0xFFFULL) != page_base) return false;
    if (p < page_base + sizeof(slab_header)) return false;
    uintptr_t offset = p - page_base - sizeof(slab_header);
    if (offset % obj_size != 0) return false;
    if (offset / obj_size >= objs_per_slab) return false;
    return true;
}

__PRIVILEGED_CODE static pmm::phys_addr_t va_to_phys(uintptr_t va) {
    return paging::get_physical(va, paging::get_kernel_pt_root());
}

__PRIVILEGED_CODE static pmm::page_frame_descriptor* va_to_pfd(uintptr_t va) {
    pmm::phys_addr_t phys = va_to_phys(va);
    if (phys == 0) return nullptr;
    return pmm::get_page_frame(phys);
}

__PRIVILEGED_CODE static slab_header* new_slab(uint8_t class_idx, heap_state* state) {
    uint16_t obj_size = state->classes[class_idx].obj_size;
    uint16_t capacity = state->classes[class_idx].objs_per_slab;

    uintptr_t page_va = 0;
    int32_t rc = vmm::alloc(1, state->page_flags, vmm::ALLOC_ZERO,
                            state->heap_tag, page_va);
    if (rc != vmm::OK) return nullptr;

    auto* pfd = va_to_pfd(page_va);
    if (!pfd) {
        vmm::free(page_va);
        return nullptr;
    }

    pfd->flags = pmm::PAGE_FLAG_SLAB | pmm::PAGE_FLAG_ALLOCATED;
    pfd->slab.class_index = class_idx;
    pfd->slab.heap_type = static_cast<uint8_t>(state->type);
    pfd->slab.free_count = capacity;
    pfd->slab.total_count = capacity;

    auto* header = reinterpret_cast<slab_header*>(page_va);
    header->magic = SLAB_MAGIC;
    header->_pad = 0;
    header->next = nullptr;
    header->prev = nullptr;

    uintptr_t obj_base = page_va + sizeof(slab_header);
    for (uint16_t i = 0; i < capacity - 1; i++) {
        void** slot = reinterpret_cast<void**>(obj_base + i * obj_size);
        *slot = reinterpret_cast<void*>(obj_base + (i + 1) * obj_size);
    }
    void** last_slot = reinterpret_cast<void**>(obj_base + (capacity - 1) * obj_size);
    *last_slot = nullptr;
    header->freelist = reinterpret_cast<void*>(obj_base);

    // Link into partial list
    slab_class& sc = state->classes[class_idx];
    header->next = sc.partial_head;
    if (sc.partial_head) {
        sc.partial_head->prev = header;
    }
    sc.partial_head = header;

    return header;
}

__PRIVILEGED_CODE static void unlink_slab(slab_header* header, slab_class& sc) {
    if (header->prev) {
        header->prev->next = header->next;
    } else {
        sc.partial_head = header->next;
    }
    if (header->next) {
        header->next->prev = header->prev;
    }
    header->next = nullptr;
    header->prev = nullptr;
}

__PRIVILEGED_CODE static void link_slab(slab_header* header, slab_class& sc) {
    header->prev = nullptr;
    header->next = sc.partial_head;
    if (sc.partial_head) {
        sc.partial_head->prev = header;
    }
    sc.partial_head = header;
}

__PRIVILEGED_CODE static void* alloc_internal(size_t size, heap_state* state) {
    if (size == 0) return nullptr;

    sync::irq_lock_guard guard(state->lock);

    // Large allocation: pass through to VMM
    if (size > LARGE_THRESHOLD) {
        size_t pages = (size + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;
        uintptr_t out = 0;
        int32_t rc = vmm::alloc(pages, state->page_flags, vmm::ALLOC_ZERO,
                                state->heap_tag, out);
        if (rc != vmm::OK) return nullptr;
        return reinterpret_cast<void*>(out);
    }

    uint8_t class_idx = size_to_class(size);
    slab_class& sc = state->classes[class_idx];

    if (!sc.partial_head) {
        if (!new_slab(class_idx, state)) return nullptr;
    }

    slab_header* header = sc.partial_head;
    if (header->magic != SLAB_MAGIC) {
        log::fatal("heap: corrupted slab magic");
    }

    void* ptr = header->freelist;
    if (!ptr) {
        log::fatal("heap: partial slab has null freelist");
    }

    uintptr_t page_base = reinterpret_cast<uintptr_t>(header);
    if (!validate_freelist_ptr(ptr, page_base, sc.obj_size, sc.objs_per_slab)) {
        log::fatal("heap: corrupted freelist pointer 0x%lx in slab 0x%lx (class %u)",
                   reinterpret_cast<uintptr_t>(ptr), page_base, class_idx);
    }

    void* next = *reinterpret_cast<void**>(ptr);
    if (next && !validate_freelist_ptr(next, page_base, sc.obj_size, sc.objs_per_slab)) {
        log::fatal("heap: corrupted freelist next 0x%lx in slab 0x%lx (class %u)",
                   reinterpret_cast<uintptr_t>(next), page_base, class_idx);
    }
    header->freelist = next;

    auto* pfd = va_to_pfd(page_base);
    pfd->slab.free_count--;

    if (pfd->slab.free_count == 0) {
        unlink_slab(header, sc);
    }

    sc.total_allocs++;

#ifdef DEBUG
    string::memset(ptr, 0, sc.obj_size);
#endif

    return ptr;
}

__PRIVILEGED_CODE static int32_t free_internal(void* ptr, heap_state* state) {
    if (!ptr) return ERR_BAD_PTR;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t page_va = addr & ~0xFFFULL;

    sync::irq_lock_guard guard(state->lock);

    auto* pfd = va_to_pfd(page_va);
    if (!pfd) return ERR_BAD_PTR;

    // Large allocation: not PAGE_FLAG_SLAB
    if (!pfd->is_slab()) {
        kva::allocation alloc;
        if (kva::query(addr, alloc) != kva::OK) return ERR_BAD_PTR;
        if (addr != alloc.base) return ERR_BAD_PTR;
        if (alloc.alloc_tag != state->heap_tag) {
            log::fatal("heap: freeing large alloc from wrong heap");
        }
        vmm::free(addr);
        return OK;
    }

    // Slab free path
    if (pfd->slab.heap_type != static_cast<uint8_t>(state->type)) {
        log::fatal("heap: freeing ptr from wrong heap (expected %u, got %u)",
                   static_cast<uint8_t>(state->type), pfd->slab.heap_type);
    }

    auto* header = reinterpret_cast<slab_header*>(page_va);
    if (header->magic != SLAB_MAGIC) {
        log::fatal("heap: corrupted slab magic on free");
    }

    uint8_t class_idx = pfd->slab.class_index;
    if (class_idx >= CLASS_COUNT) {
        log::fatal("heap: corrupted class_index %u on free", class_idx);
    }

    slab_class& sc = state->classes[class_idx];

    if (!validate_freelist_ptr(ptr, page_va, sc.obj_size, sc.objs_per_slab)) {
        log::fatal("heap: invalid free pointer 0x%lx in slab 0x%lx (class %u)",
                   addr, page_va, class_idx);
    }

    if (pfd->slab.free_count >= pfd->slab.total_count) {
        log::fatal("heap: double-free detected at 0x%lx in slab 0x%lx (class %u, free=%u total=%u)",
                   addr, page_va, class_idx, pfd->slab.free_count, pfd->slab.total_count);
    }

    bool was_full = (pfd->slab.free_count == 0);

#ifdef DEBUG
    string::memset(ptr, 0xDE, sc.obj_size);
#endif

    *reinterpret_cast<void**>(ptr) = header->freelist;
    header->freelist = ptr;
    pfd->slab.free_count++;

    if (was_full) {
        link_slab(header, sc);
    }

    if (pfd->slab.free_count == pfd->slab.total_count) {
        unlink_slab(header, sc);
        header->magic = 0;
        pfd->flags = pmm::PAGE_FLAG_ALLOCATED;
        pfd->buddy.order = 0;
        vmm::free(page_va);
    }

    sc.total_frees++;
    return OK;
}

__PRIVILEGED_CODE static void init_heap_state(heap_state& state, paging::page_flags_t flags,
                            kva::tag tag, heap_type kind) {
    state.lock = sync::SPINLOCK_INIT;
    state.page_flags = flags;
    state.heap_tag = tag;
    state.type = kind;
    for (uint8_t i = 0; i < CLASS_COUNT; i++) {
        state.classes[i].partial_head = nullptr;
        state.classes[i].obj_size = CLASS_SIZES[i];
        state.classes[i].objs_per_slab =
            static_cast<uint16_t>((pmm::PAGE_SIZE - sizeof(slab_header)) / CLASS_SIZES[i]);
        state.classes[i].total_allocs = 0;
        state.classes[i].total_frees = 0;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    init_heap_state(g_priv_heap,
                    paging::PAGE_KERNEL_RW,
                    kva::tag::privileged_heap,
                    heap_type::privileged);

    init_heap_state(g_unpriv_heap,
                    paging::PAGE_USER_RW,
                    kva::tag::unprivileged_heap,
                    heap_type::unprivileged);

    log::info("heap: initialized (priv + unpriv, %u size classes: 16..2048)",
              CLASS_COUNT);

#if 0
    for (uint8_t i = 0; i < CLASS_COUNT; i++) {
        log::debug("heap: class %u: %u bytes, %u objs/slab",
                   i, g_priv_heap.classes[i].obj_size,
                   g_priv_heap.classes[i].objs_per_slab);
    }
#endif

    return OK;
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE void* kalloc(size_t size) {
    return alloc_internal(size, &g_priv_heap);
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE void* kzalloc(size_t size) {
    void* p = alloc_internal(size, &g_priv_heap);
    if (p && size <= LARGE_THRESHOLD) {
        string::memset(p, 0, size);
    }
    return p;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t kfree(void* ptr) {
    return free_internal(ptr, &g_priv_heap);
}

[[nodiscard]] void* ualloc(size_t size) {
    void* p = nullptr;
    RUN_ELEVATED(p = alloc_internal(size, &g_unpriv_heap));
    return p;
}

[[nodiscard]] void* uzalloc(size_t size) {
    void* p = nullptr;
    RUN_ELEVATED({
        p = alloc_internal(size, &g_unpriv_heap);
        if (p && size <= LARGE_THRESHOLD) {
            string::memset(p, 0, size);
        }
    });
    return p;
}

int32_t ufree(void* ptr) {
    int32_t rc = ERR_BAD_PTR;
    RUN_ELEVATED(rc = free_internal(ptr, &g_unpriv_heap));
    return rc;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_stats() {
    auto dump_heap = [](const char* name, heap_state& state) {
        sync::irq_lock_guard guard(state.lock);
        log::info("heap: %s heap:", name);
        for (uint8_t i = 0; i < CLASS_COUNT; i++) {
            const slab_class& sc = state.classes[i];
            if (sc.total_allocs > 0 || sc.partial_head) {
                uint32_t partial_count = 0;
                for (auto* s = sc.partial_head; s; s = s->next) partial_count++;
                log::info("  class %u (%u B): %lu allocs, %lu frees, %u partial slabs",
                          i, sc.obj_size, sc.total_allocs, sc.total_frees, partial_count);
            }
        }
    };
    dump_heap("privileged", g_priv_heap);
    dump_heap("unprivileged", g_unpriv_heap);
}

} // namespace heap
