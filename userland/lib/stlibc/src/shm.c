#include <stlibc/ipc/shm.h>
#include <stlibc/stellux_syscalls.h>

shm_handle_t stlx_shm_create(const char* name, size_t size, int access_policy) {
    long result = syscall3(SYS_SHM_CREATE, 
                                   (uint64_t)name, 
                                   (uint64_t)size, 
                                   (uint64_t)access_policy);
    
    // Syscall returns handle on success, negative errno on failure
    if (result < 0) {
        return 0; // Return 0 to indicate failure
    }
    
    return (shm_handle_t)result;
}

shm_handle_t stlx_shm_open(const char* name) {
    long result = syscall1(SYS_SHM_OPEN, (uint64_t)name);
    
    // Syscall returns handle on success, negative errno on failure
    if (result < 0) {
        return 0; // Return 0 to indicate failure
    }
    
    return (shm_handle_t)result;
}

int stlx_shm_destroy(shm_handle_t handle) {
    long result = syscall1(SYS_SHM_DESTROY, (uint64_t)handle);
    
    // Syscall returns 0 on success, negative errno on failure
    return (int)result;
}

void* stlx_shm_map(shm_handle_t handle, uint64_t flags) {
    long result = syscall2(SYS_SHM_MAP, 
                                   (uint64_t)handle, 
                                   (uint64_t)flags);
    
    // Syscall returns address on success, negative errno on failure
    if (result < 0) {
        return NULL; // Return NULL to indicate failure
    }
    
    return (void*)result;
}

int stlx_shm_unmap(shm_handle_t handle, void* addr) {
    long result = syscall2(SYS_SHM_UNMAP, 
                                   (uint64_t)handle, 
                                   (uint64_t)addr);
    
    // Syscall returns 0 on success, negative errno on failure
    return (int)result;
}
