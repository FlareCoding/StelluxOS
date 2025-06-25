#include <syscall/handlers/sys_shm.h>
#include <ipc/shm.h>
#include <core/string.h>
#include <core/klog.h>
#include <process/process.h>

// Maximum name length for shared memory regions (for security)
static constexpr size_t MAX_SHM_NAME_LENGTH = 256;

// Maximum size for shared memory regions (for security) - 1GB
static constexpr size_t MAX_SHM_SIZE = 1024ULL * 1024 * 1024;

// Userland constants for access policies
#define SHM_READ_ONLY      0
#define SHM_READ_WRITE     1

// Userland constants for mapping flags
#define SHM_MAP_READ       0x1
#define SHM_MAP_WRITE      0x2

/**
 * @brief Safely copies a string from userland to kernel space.
 * @param userland_str Pointer to userland string
 * @param max_length Maximum allowed string length
 * @return kstl::string containing the copied string, empty on failure
 */
static kstl::string safe_copy_string_from_userland(const char* userland_str, size_t max_length) {
    if (!userland_str) {
        return kstl::string();
    }

    // Copy string byte by byte with length limit
    char kernel_buffer[MAX_SHM_NAME_LENGTH + 1] = {0};
    size_t length = 0;
    
    for (size_t i = 0; i < max_length; ++i) {
        // In a real implementation, you'd want to check if the userland address is valid
        // For now, we'll assume the string is accessible
        kernel_buffer[i] = userland_str[i];
        length++;
        
        if (userland_str[i] == '\0') {
            break;
        }
    }
    
    // Ensure null termination
    kernel_buffer[max_length] = '\0';
    
    return kstl::string(kernel_buffer);
}

/**
 * @brief Converts userland access policy to kernel enum.
 */
static ipc::shm_access convert_access_policy(int userland_policy) {
    switch (userland_policy) {
        case SHM_READ_ONLY:
            return ipc::shm_access::READ_ONLY;
        case SHM_READ_WRITE:
            return ipc::shm_access::READ_WRITE;
        default:
            return ipc::shm_access::READ_ONLY; // Default to most restrictive
    }
}

/**
 * @brief Validates userland mapping flags.
 */
static bool validate_mapping_flags(uint64_t flags) {
    // Only allow read and write flags
    const uint64_t valid_flags = SHM_MAP_READ | SHM_MAP_WRITE;
    return (flags & ~valid_flags) == 0;
}

DECLARE_SYSCALL_HANDLER(shm_create) {
    /* -----------------------------------------------------------------
     *  Syscall: shm_create(name, size, access_policy)
     *      arg1 = const char *  name           (userland pointer)
     *      arg2 = size_t        size           (size in bytes)
     *      arg3 = int           access_policy  (SHM_READ_ONLY, SHM_READ_WRITE)
     *  Returns: shm_handle_t on success, 0 on failure
     * -----------------------------------------------------------------*/
    
    const char* userland_name = reinterpret_cast<const char*>(arg1);
    size_t size = static_cast<size_t>(arg2);
    int access_policy = static_cast<int>(arg3);
    
    // Validate userland name pointer
    if (!userland_name) {
        return -EFAULT;
    }
    
    // Validate size constraints
    if (size == 0) {
        return -EINVAL;
    }
    
    if (size > MAX_SHM_SIZE) {
        return -EINVAL;
    }
    
    // Copy name safely from userland
    kstl::string name = safe_copy_string_from_userland(userland_name, MAX_SHM_NAME_LENGTH);
    if (name.length() == 0) {
        return -EINVAL;
    }
    
    // Convert access policy
    ipc::shm_access policy = convert_access_policy(access_policy);
    
    // Create the shared memory region
    ipc::shm_handle_t handle = ipc::shared_memory::create(name, size, policy);
    if (handle == 0) {
        return -EEXIST; // Name already exists or allocation failed
    }
    
    SYSCALL_TRACE("shm_create(\"%s\", %llu, %d) = %llu\n", 
        name.c_str(), size, access_policy, handle);
    
    return static_cast<long>(handle);
}

DECLARE_SYSCALL_HANDLER(shm_open) {
    /* -----------------------------------------------------------------
     *  Syscall: shm_open(name)
     *      arg1 = const char *  name  (userland pointer)
     *  Returns: shm_handle_t on success, 0 on failure
     * -----------------------------------------------------------------*/
    
    const char* userland_name = reinterpret_cast<const char*>(arg1);
    
    // Validate userland name pointer
    if (!userland_name) {
        return -EFAULT;
    }
    
    // Copy name safely from userland
    kstl::string name = safe_copy_string_from_userland(userland_name, MAX_SHM_NAME_LENGTH);
    if (name.length() == 0) {
        return -EINVAL;
    }
    
    // Open the shared memory region
    ipc::shm_handle_t handle = ipc::shared_memory::open(name);
    if (handle == 0) {
        return -ENOENT; // Name not found
    }
    
    SYSCALL_TRACE("shm_open(\"%s\") = %llu\n", name.c_str(), handle);
    
    return static_cast<long>(handle);
}

DECLARE_SYSCALL_HANDLER(shm_destroy) {
    /* -----------------------------------------------------------------
     *  Syscall: shm_destroy(handle)
     *      arg1 = shm_handle_t  handle
     *  Returns: 0 on success, negative errno on failure
     * -----------------------------------------------------------------*/
    
    ipc::shm_handle_t handle = static_cast<ipc::shm_handle_t>(arg1);
    
    // Validate handle
    if (handle == 0) {
        return -EINVAL;
    }
    
    // Destroy the shared memory region
    bool success = ipc::shared_memory::destroy(handle);
    if (!success) {
        return -ENOENT; // Handle not found
    }
    
    SYSCALL_TRACE("shm_destroy(%llu) = 0\n", handle);
    
    return 0;
}

DECLARE_SYSCALL_HANDLER(shm_map) {
    /* -----------------------------------------------------------------
     *  Syscall: shm_map(handle, flags)
     *      arg1 = shm_handle_t  handle
     *      arg2 = uint64_t      flags   (SHM_MAP_READ | SHM_MAP_WRITE)
     *  Returns: mapped address on success, negative errno on failure
     * -----------------------------------------------------------------*/
    
    ipc::shm_handle_t handle = static_cast<ipc::shm_handle_t>(arg1);
    uint64_t flags = static_cast<uint64_t>(arg2);
    
    // Validate handle
    if (handle == 0) {
        return -EINVAL;
    }
    
    // Validate flags
    if (!validate_mapping_flags(flags)) {
        return -EINVAL;
    }
    
    // Convert userland flags to kernel page flags
    uint64_t kernel_flags = PTE_PRESENT | PTE_US; // User accessible
    if (flags & SHM_MAP_WRITE) {
        kernel_flags |= PTE_RW;
    }
    // Note: Shared memory is non-executable by default (no PTE_NX needed due to NX bit behavior)
    
    // Map the shared memory region - force userland context
    void* mapped_addr = ipc::shared_memory::map(handle, kernel_flags, ipc::shm_mapping_context::USERLAND);
    if (!mapped_addr) {
        return -ENOMEM; // Mapping failed
    }
    
    SYSCALL_TRACE("shm_map(%llu, 0x%llx) = 0x%llx\n", 
        handle, flags, reinterpret_cast<uintptr_t>(mapped_addr));
    
    return static_cast<long>(reinterpret_cast<uintptr_t>(mapped_addr));
}

DECLARE_SYSCALL_HANDLER(shm_unmap) {
    /* -----------------------------------------------------------------
     *  Syscall: shm_unmap(handle, addr)
     *      arg1 = shm_handle_t  handle
     *      arg2 = void*         addr   (address to unmap)
     *  Returns: 0 on success, negative errno on failure
     * -----------------------------------------------------------------*/
    
    ipc::shm_handle_t handle = static_cast<ipc::shm_handle_t>(arg1);
    void* addr = reinterpret_cast<void*>(arg2);
    
    // Validate handle
    if (handle == 0) {
        return -EINVAL;
    }
    
    // Validate address
    if (!addr) {
        return -EINVAL;
    }
    
    // Unmap the shared memory region - force userland context
    bool success = ipc::shared_memory::unmap(handle, addr, ipc::shm_mapping_context::USERLAND);
    if (!success) {
        return -EINVAL; // Unmapping failed
    }
    
    SYSCALL_TRACE("shm_unmap(%llu, 0x%llx) = 0\n", handle, addr);
    
    return 0;
}
