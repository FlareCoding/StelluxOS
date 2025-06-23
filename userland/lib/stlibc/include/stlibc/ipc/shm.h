#ifndef STLIBC_IPC_SHM_H
#define STLIBC_IPC_SHM_H

#include <stlibc/stlibcdef.h>

// Shared memory handle type
typedef uint64_t shm_handle_t;

// Access policies for shared memory creation
#define SHM_READ_ONLY      0
#define SHM_READ_WRITE     1

// Mapping flags for shared memory mapping
#define SHM_MAP_READ       0x1
#define SHM_MAP_WRITE      0x2

/**
 * @brief Creates a new shared memory region.
 * 
 * @param name Name of the shared memory region (max 256 characters)
 * @param size Size of the region in bytes (max 1GB)
 * @param access_policy Access policy (SHM_READ_ONLY or SHM_READ_WRITE)
 * @return shm_handle_t Handle to the shared memory region, 0 on failure
 * 
 * Creates a new shared memory region with the specified name and size.
 * The name must be unique system-wide. If a region with the same name
 * already exists, this function will fail.
 * 
 * Example:
 * ```c
 * shm_handle_t handle = stlx_shm_create("my_buffer", 4096, SHM_READ_WRITE);
 * if (handle == 0) {
 *     // Handle error
 * }
 * ```
 */
shm_handle_t stlx_shm_create(const char* name, size_t size, int access_policy);

/**
 * @brief Opens an existing shared memory region by name.
 * 
 * @param name Name of the shared memory region to open
 * @return shm_handle_t Handle to the shared memory region, 0 on failure
 * 
 * Opens an existing shared memory region that was previously created
 * with stlx_shm_create(). Multiple processes can open the same region.
 * 
 * Example:
 * ```c
 * shm_handle_t handle = stlx_shm_open("my_buffer");
 * if (handle == 0) {
 *     // Region doesn't exist or other error
 * }
 * ```
 */
shm_handle_t stlx_shm_open(const char* name);

/**
 * @brief Marks a shared memory region for destruction.
 * 
 * @param handle Handle to the shared memory region
 * @return int 0 on success, negative value on error
 * 
 * Marks the shared memory region for deletion. The actual resources
 * will be freed when all mappings are unmapped and all handles are closed.
 * After calling this function, no new mappings can be created, but
 * existing mappings remain valid until explicitly unmapped.
 * 
 * Example:
 * ```c
 * if (stlx_shm_destroy(handle) != 0) {
 *     // Handle error
 * }
 * ```
 */
int stlx_shm_destroy(shm_handle_t handle);

/**
 * @brief Maps a shared memory region into the process address space.
 * 
 * @param handle Handle to the shared memory region
 * @param flags Mapping flags (SHM_MAP_READ | SHM_MAP_WRITE)
 * @return void* Pointer to the mapped memory, or NULL on failure
 * 
 * Maps the shared memory region into the current process's address space.
 * The flags specify the desired access permissions. The mapped memory
 * is shared with other processes that map the same region.
 * 
 * Example:
 * ```c
 * void* ptr = stlx_shm_map(handle, SHM_MAP_READ | SHM_MAP_WRITE);
 * if (ptr == NULL) {
 *     // Handle mapping error
 * }
 * 
 * // Use the memory
 * memset(ptr, 0x42, 4096);
 * ```
 */
void* stlx_shm_map(shm_handle_t handle, uint64_t flags);

/**
 * @brief Unmaps a shared memory region from the process address space.
 * 
 * @param handle Handle to the shared memory region
 * @param addr Pointer to the mapped memory (returned by stlx_shm_map)
 * @return int 0 on success, negative value on error
 * 
 * Unmaps the shared memory region from the current process's address space.
 * After unmapping, the memory should not be accessed. Other processes
 * that have mapped the same region are not affected.
 * 
 * Example:
 * ```c
 * if (stlx_shm_unmap(handle, ptr) != 0) {
 *     // Handle unmapping error
 * }
 * ```
 */
int stlx_shm_unmap(shm_handle_t handle, void* addr);

#endif // STLIBC_IPC_SHM_H
