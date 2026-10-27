// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"
#include "hw/mmio.h"

namespace drivers::nvidia {

instance_memory::instance_memory(video_memory& memory, firmware& fw)
    : m_memory(memory)
    , m_fw(fw)
    , m_range{} {}

int32_t instance_memory::set_up() {
    vram_range allocated = {};
    int32_t rc = m_memory.allocate(INSTANCE_MEMORY_BYTES, INSTANCE_MEMORY_ALIGNMENT, vram_use::general, allocated);
    if (rc != OK) {
        log::error(
            "nvidia: allocating 0x%lX bytes of video memory at alignment 0x%lX for the display's instance memory "
            "failed: %d",
            INSTANCE_MEMORY_BYTES,
            INSTANCE_MEMORY_ALIGNMENT,
            rc
        );
        return rc;
    }

    cpu_mapping mapping = {};
    rc = m_memory.map_for_cpu(allocated, mapping);
    if (rc != OK) {
        log::error(
            "nvidia: mapping the display's instance memory at 0x%lX in video memory for the CPU to clear it failed: "
            "%d, so the memory is released",
            allocated.offset,
            rc
        );
        m_memory.release(allocated);
        return rc;
    }

    for (uint64_t offset = 0; offset < INSTANCE_MEMORY_BYTES; offset += sizeof(uint64_t)) {
        mmio::write64(mapping.virt + offset, 0);
    }

    m_memory.unmap_for_cpu(allocated);
    m_range = allocated;

    rm::display_instance_memory_params params = {};
    params.address = allocated.offset;
    params.bytes = INSTANCE_MEMORY_BYTES;
    params.address_space = rm::ADDRESS_SPACE_VIDEO_MEMORY;
    params.cpu_caching = rm::CPU_CACHING_WRITE_COMBINED;

    rc = m_fw.internal_control(rm::CONTROL_WRITE_DISPLAY_INSTANCE_MEMORY, &params, sizeof(params));
    if (rc != OK) {
        log::error(
            "nvidia: telling the GSP's firmware where the display's instance memory lies, at 0x%lX in video memory, "
            "failed: %d, so the memory stays allocated",
            allocated.offset,
            rc
        );
        return rc;
    }

    return OK;
}

const vram_range& instance_memory::range() const {
    return m_range;
}

} // namespace drivers::nvidia
