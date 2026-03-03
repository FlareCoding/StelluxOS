#include "resource/providers/shm_provider.h"
#include "resource/providers/shmem_resource_provider.h"
#include "common/hash.h"
#include "common/hashmap.h"
#include "common/string.h"
#include "fs/fstypes.h"
#include "mm/heap.h"
#include "mm/shmem.h"
#include "rc/strong_ref.h"
#include "sync/mutex.h"
#include "sync/spinlock.h"

namespace resource::shm_provider {

namespace {

constexpr size_t SHM_PREFIX_LEN = 9; // strlen("/dev/shm/")
constexpr size_t SHM_REGISTRY_BUCKETS = 32;

struct shm_entry {
    char name[fs::NAME_MAX + 1];
    rc::strong_ref<mm::shmem> backing;
    hashmap::node hash_link;
};

struct shm_key_ops {
    using key_type = const char*;
    static key_type key_of(const shm_entry& e) { return e.name; }
    static uint64_t hash(const key_type& k) { return hash::string(k); }
    static bool equal(const key_type& a, const key_type& b) {
        return string::strcmp(a, b) == 0;
    }
};

using shm_map = hashmap::map<shm_entry, &shm_entry::hash_link, shm_key_ops>;

__PRIVILEGED_DATA sync::spinlock g_shm_lock = sync::SPINLOCK_INIT;
__PRIVILEGED_BSS hashmap::bucket g_shm_buckets[SHM_REGISTRY_BUCKETS];
__PRIVILEGED_BSS shm_map g_shm_registry;
__PRIVILEGED_BSS bool g_shm_inited;

void ensure_init() {
    if (!g_shm_inited) {
        g_shm_registry.init(g_shm_buckets, SHM_REGISTRY_BUCKETS);
        g_shm_inited = true;
    }
}

bool extract_shm_name(
    const char* path, const char** out_name, size_t* out_len
) {
    if (string::strncmp(path, "/dev/shm", 8) != 0) {
        return false;
    }
    if (path[8] != '/' && path[8] != '\0') {
        return false;
    }
    if (path[8] == '\0') {
        return false;
    }

    const char* name = path + SHM_PREFIX_LEN;
    size_t len = 0;
    while (name[len] != '\0' && name[len] != '/') {
        len++;
    }
    if (name[len] == '/') {
        return false;
    }
    if (len == 0) {
        return false;
    }
    if (len > fs::NAME_MAX) {
        return false;
    }

    *out_name = name;
    *out_len = len;
    return true;
}

} // namespace

bool is_shm_path(const char* path) {
    if (!path) {
        return false;
    }
    if (string::strncmp(path, "/dev/shm", 8) != 0) {
        return false;
    }
    return path[8] == '/' || path[8] == '\0';
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t open_shm_resource(
    const char* path,
    uint32_t flags,
    resource_object** out_obj
) {
    if (!path || !out_obj) {
        return ERR_INVAL;
    }

    const char* name = nullptr;
    size_t name_len = 0;
    if (!extract_shm_name(path, &name, &name_len)) {
        return ERR_INVAL;
    }

    bool create = (flags & fs::O_CREAT) != 0;
    bool excl = create && (flags & fs::O_EXCL) != 0;

    rc::strong_ref<mm::shmem> backing_ref;
    bool need_trunc = false;

    {
        sync::irq_lock_guard guard(g_shm_lock);
        ensure_init();

        shm_entry* existing = g_shm_registry.find(name);

        if (existing) {
            if (excl) {
                return ERR_EXIST;
            }
            backing_ref = existing->backing;
            need_trunc = (flags & fs::O_TRUNC) != 0;
        } else if (!create) {
            return ERR_NOENT;
        } else {
            mm::shmem* raw = mm::shmem_create(0);
            if (!raw) {
                return ERR_NOMEM;
            }

            auto* entry = heap::kalloc_new<shm_entry>();
            if (!entry) {
                mm::shmem::ref_destroy(raw);
                return ERR_NOMEM;
            }

            string::memcpy(entry->name, name, name_len);
            entry->name[name_len] = '\0';
            entry->backing = rc::strong_ref<mm::shmem>::adopt(raw);
            entry->hash_link = {};

            g_shm_registry.insert(entry);

            backing_ref = entry->backing;
        }
    }

    if (need_trunc) {
        sync::mutex_lock(backing_ref->lock);
        mm::shmem_resize_locked(backing_ref.ptr(), 0);
        sync::mutex_unlock(backing_ref->lock);
    }

    return shmem_resource_provider::create_shmem_resource_with_backing(
        backing_ref.ptr(), flags, out_obj);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t unlink_shm(const char* path) {
    if (!path) {
        return ERR_INVAL;
    }

    const char* name = nullptr;
    size_t name_len = 0;
    if (!extract_shm_name(path, &name, &name_len)) {
        return ERR_INVAL;
    }

    sync::irq_lock_guard guard(g_shm_lock);
    ensure_init();

    shm_entry* entry = g_shm_registry.find(name);
    if (!entry) {
        return ERR_NOENT;
    }

    g_shm_registry.remove(*entry);
    heap::kfree_delete(entry);
    return OK;
}

} // namespace resource::shm_provider
