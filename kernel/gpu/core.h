#ifndef STELLUX_GPU_CORE_H
#define STELLUX_GPU_CORE_H

#include "common/types.h"
#include "fs/node.h"
#include "gpu/gpu.h"
#include "mm/device_backing.h"
#include "resource/resource.h"

namespace gpu {

/**
 * One registered GPU, published as the device node /dev/gpuN, whose file takes every ioctl call gpu_abi.h defines. It
 * lives on the privileged heap for good, since no backend unregisters.
 */
class device final : public fs::node {
public:
    device(const char* name, backend* b, const device_facts& facts);

    int32_t ioctl(fs::file* f, uint32_t cmd, uint64_t arg) override;

    backend& get_backend() const { return *m_backend; }
    const gpu_info& info() const { return m_facts.info; }

private:
    backend*     m_backend;
    device_facts m_facts;
};

/**
 * GPU memory the core counts as one object. The resource object its handles name and each CPU mapping of it hold a
 * reference, and after the last one drops the reaper returns the memory to the device's backend and frees the buffer.
 * It lives on the unprivileged heap, and its device outlives it.
 */
class buffer final : public mm::device_backing {
public:
    buffer(device& owner, uint64_t size, uint32_t flags) : m_device(owner), m_size(size), m_flags(flags) {}

    device& owner() const { return m_device; }
    uint64_t size() const { return m_size; }
    uint32_t flags() const { return m_flags; }
    memory* backend_memory() const { return m_memory; }

    // Hands the buffer the memory its backend created for it, returned to the backend after the last reference drops
    void take(memory* mem) { m_memory = mem; }

private:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void release_memory() override;

    device&  m_device;
    memory*  m_memory = nullptr;
    uint64_t m_size;
    uint32_t m_flags;
};

/**
 * @brief Translate a backend's code into the file layer's, any code outside the backend's contract as ERR_IO.
 */
int32_t to_fs_error(int32_t rc);

/**
 * @brief Copy `bytes` in from the user address `src`.
 * @return fs::OK, or fs::ERR_INVAL when the kernel cannot read them.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_in(void* dst, uint64_t src, size_t bytes);

/**
 * @brief Copy `bytes` out to the user address `dst`.
 * @return fs::OK, or fs::ERR_INVAL when the kernel cannot write them.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_out(uint64_t dst, const void* src, size_t bytes);

/**
 * @brief Install a handle to `obj` with `rights` in the calling process and write it to the user address
 * `user_handle`, closing the handle again when the write fails. The handle takes a reference of its own.
 * @return fs::OK, fs::ERR_NOMEM when the handle table is full or cannot grow, or fs::ERR_INVAL when the write fails.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t install_handle(resource::resource_object* obj, uint32_t rights, uint64_t user_handle);

// Each call below takes the user address of its argument and returns a file layer code, as gpu_abi.h's rules give

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_buffer(device& dev, uint64_t arg);

} // namespace gpu

#endif // STELLUX_GPU_CORE_H
