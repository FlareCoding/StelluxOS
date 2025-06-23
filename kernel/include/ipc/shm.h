#ifndef SHM_H
#define SHM_H
#include <sync.h>
#include <core/string.h>
#include <kstl/hashmap.h>
#include <kstl/vector.h>

namespace ipc {

using shm_handle_t = uint64_t;

/**
 * @enum shm_access
 * @brief Describes the default access policy for a shared memory region.
 */
enum class shm_access {
    READ_ONLY,      // Region is read-only.
    READ_WRITE,     // Region is readable and writable.
};

/**
 * @enum shm_mapping_context
 * @brief Describes the context in which shared memory is being mapped.
 */
enum class shm_mapping_context {
    KERNEL,     // Kernel thread mapping - use vmm directly
    USERLAND    // Userland process mapping - use VMA system
};

/**
 * @struct shm_region
 * @brief Internal metadata for a shared memory region.
 */
struct shm_region {
    shm_handle_t                   id;              // Unique identifier for the region.
    kstl::string                   name;            // Optional name for lookup.
    size_t                         size;            // Size in bytes of the region.
    shm_access                     policy;          // Default access policy.
    kstl::vector<uintptr_t>        pages;           // Physical pages backing the region.
    size_t                         ref_count;       // Reference count across mappings.
    bool                           pending_delete;  // Marked for deletion when ref_count reaches zero.
    mutex                          lock;            // Protects region metadata.
};

/**
 * @class shared_memory
 * @brief Manager for shared memory regions.
 */
class shared_memory {
public:
    /** Create a new shared memory region. */
    static shm_handle_t create(const kstl::string& name, size_t size, shm_access policy);

    /** Open an existing shared memory region by name. */
    static shm_handle_t open(const kstl::string& name);

    /** Mark a region for destruction. Resources free once unmapped. */
    static bool destroy(shm_handle_t handle);

    /** Map a shared memory region into the caller address space. */
    static void* map(shm_handle_t handle, uint64_t flags, shm_mapping_context context);

    /** Unmap a previously mapped shared memory region. */
    static bool unmap(shm_handle_t handle, void* addr, shm_mapping_context context);

private:
    static shm_region* get_region(shm_handle_t handle);

    static shm_handle_t                           s_next_id;
    static mutex                                  s_global_lock;
    static kstl::hashmap<shm_handle_t, kstl::shared_ptr<shm_region>> s_regions;
    static kstl::hashmap<kstl::string, shm_handle_t>                s_name_map;
};

} // namespace ipc

#endif // SHM_H
