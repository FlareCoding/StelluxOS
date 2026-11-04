#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/copy_channel.h"
#include "drivers/graphics/nvidia/engine_table.h"
#include "drivers/graphics/nvidia/failed_mark.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "gpu/gpu.h"
#include "sync/atomic.h"

namespace drivers::nvidia {

// The card as its PCI function and the firmware's static configuration describe it, which the GPU core reports
struct card_identity {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint8_t  revision;
    uint32_t gpc_count;
    uint32_t tpc_count;
    char     name[GPU_NAME_BYTES]; // NUL terminated
};

/**
 * The GPU's engines as the driver task brings them up once `attach()` has
 * returned `OK`: the engine table, the kernel's copy channel on the first
 * asynchronous copy engine, and that engine's nonstall vector. The driver
 * object holds one for its life, and once `register_gpu` succeeds the GPU core
 * drives the engines as the card's `gpu::backend`.
 *
 * `bring_up` waits for the firmware and for video memory, so the driver task
 * must call it, and only once.
 *
 * After a timeout the hung channel and the memory it fills stay for good.
 * After another error, only what a refused release or a failed unmap leaves
 * the firmware or the GPU able to reach stays.
 */
class engines : public gpu::backend {
public:
    // Engines holding nothing, using `fw`, `objects`, `memory` and `failed`, which must outlive them
    engines(firmware& fw, const gpu_objects& objects, video_memory& memory, failed_mark& failed);

    engines(const engines&) = delete;
    engines& operator=(const engines&) = delete;

    /**
     * @brief Read the engine table, create the kernel's copy channel on the
     * first asynchronous copy engine with `bar0` for its doorbell, enable the
     * engine's nonstall vector in `chip`'s leaves, and check the channel by
     * having it zero and then fill video memory, each read back through BAR1.
     * The engines keep `chip`, which must outlive them.
     * @return `OK`, `ERR_BAD_STATE` when no asynchronous copy engine exists or
     * a word reads back wrong, `ERR_TIMEOUT` when a copy hangs the channel, or
     * the failing step's error. Every error is logged and leaves the vector
     * masked.
     */
    int32_t bring_up(registers& bar0, const dma::device_profile& card, const chip_info& chip);

    /**
     * @brief Register the engines with the GPU core as the next /dev/gpuN,
     * describing the card by `identity`. The driver task must call it once,
     * after `bring_up` returned `OK`. The core keeps the engines for good.
     * @param identity The card's identity, which `read_facts` reports.
     * @return `OK`, or `ERR_NO_MEMORY`, logged, when the core cannot publish
     * the device, which leaves the display running without one.
     */
    int32_t register_gpu(const card_identity& identity);

    void read_facts(gpu::device_facts& out) override;
    void read_memory(gpu_memory& out) override;
    int32_t read_timestamp(uint64_t& gpu_ns) override;

    int32_t create_memory(
        uint64_t size,
        uint32_t placement,
        uint32_t flags,
        gpu::space* s,
        uint64_t backend_id,
        gpu::memory** out,
        uint32_t* out_device_id
    ) override;
    int32_t wrap_pages(
        const pmm::phys_addr_t* pages,
        size_t count,
        mm::shmem_mapping&& pin,
        gpu::memory** out,
        uint32_t* out_device_id
    ) override;
    int32_t view_for_cpu(gpu::memory* mem, gpu::cpu_view& out) override;
    void release_memory(gpu::memory* mem) override;

    int32_t create_space(gpu::context* core, gpu::space** out) override;
    void destroy_space(gpu::space* s) override;
    int32_t map(
        gpu::space* s,
        uint64_t va,
        gpu::memory* mem,
        uint64_t offset,
        uint64_t size,
        uint32_t kind,
        bool read_only
    ) override;
    int32_t unmap(gpu::space* s, uint64_t va, uint64_t size) override;

    int32_t create_queue(gpu::space* s, uint32_t kind, gpu::core_queue* core, gpu::queue** out) override;
    void stop_queue(gpu::queue* q) override;
    void destroy_queue(gpu::queue* q) override;
    int32_t check_commands(const void* commands, size_t size) override;
    void run(gpu::queue* q, uint64_t sequence, const void* commands, size_t size) override;

    /**
     * @brief Clear the copy engine's nonstall vector when it is pending and
     * have the copy channel read its semaphore, from the card's interrupt
     * handler. Does nothing before `bring_up` creates the copy channel or after
     * it masks the vector. Takes the copy channel's spinlock and never sleeps.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void service_interrupt(registers& bar0);

private:
    friend struct engines_test_access;

    // Fills the check's memory, reads it back and logs each copy's time, keeping the memory for good after a timeout
    int32_t check_copy_channel();

    // Has the copy channel fill the checked bytes with `value`, then reads them and the page after them back
    int32_t fill_and_compare(const vram_range& range, uintptr_t virt, uint32_t value, uint64_t& took_ns);

    firmware&          m_firmware;
    const gpu_objects& m_objects;
    video_memory&      m_memory;
    failed_mark&       m_failed;
    engine_table       m_table;

    registers          m_bar0{0};          // the card's registers, set by `bring_up`
    const chip_info*   m_chip = nullptr;   // set by `bring_up`
    card_identity      m_identity = {};    // set by `register_gpu`
    gpu::device*       m_device = nullptr; // the core's device once `register_gpu` succeeds

    // The kernel's copy channel in `m_copy_channel_bytes`, never destructed, since the handler may still reach it
    copy_channel*                 m_copy_channel = nullptr;
    alignas(copy_channel) uint8_t m_copy_channel_bytes[sizeof(copy_channel)];

    uint32_t           m_copy_vector = 0;
    sync::atomic<bool> m_is_copy_vector_on{false}; // stored with release once the channel exists, loaded with acquire
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H
