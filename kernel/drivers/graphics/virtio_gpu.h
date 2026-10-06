#ifndef STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H
#define STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H

#include "drivers/virtio/virtio_pci_driver.h"
#include "drivers/graphics/gfxfb.h"
#include "sync/mutex.h"

namespace drivers::virtio {

constexpr uint16_t VIRTIO_DEV_GPU = VIRTIO_DEV_MODERN_BASE + 16;

constexpr uint16_t VIRTIO_GPU_QUEUE_CONTROL = 0;

constexpr uint32_t VIRTIO_GPU_CMD_GET_DISPLAY_INFO        = 0x0100;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      = 0x0101;
constexpr uint32_t VIRTIO_GPU_CMD_SET_SCANOUT             = 0x0103;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_FLUSH          = 0x0104;
constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     = 0x0105;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;

constexpr uint32_t VIRTIO_GPU_RESP_OK_NODATA       = 0x1100;
constexpr uint32_t VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101;
constexpr uint32_t VIRTIO_GPU_RESP_ERR_UNSPEC      = 0x1200;

constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2;

constexpr uint32_t VIRTIO_GPU_MAX_SCANOUTS = 16;

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    virtio_gpu_ctrl_hdr    hdr;
    virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

// A single entry, enough for a physically contiguous backing
struct virtio_gpu_resource_attach_backing {
    virtio_gpu_ctrl_hdr  hdr;
    uint32_t             resource_id;
    uint32_t             nr_entries;
    virtio_gpu_mem_entry entry;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    virtio_gpu_ctrl_hdr hdr;
    virtio_gpu_rect     r;
    uint32_t            scanout_id;
    uint32_t            resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    virtio_gpu_ctrl_hdr hdr;
    virtio_gpu_rect     r;
    uint64_t            offset;
    uint32_t            resource_id;
    uint32_t            padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    virtio_gpu_ctrl_hdr hdr;
    virtio_gpu_rect     r;
    uint32_t            resource_id;
    uint32_t            padding;
} __attribute__((packed));

} // namespace drivers::virtio

namespace drivers {

/**
 * A virtio GPU used as a 2D display. Userland draws into the RAM backing its
 * one resource, and `flush()` copies the changed rects into it and shows them.
 */
class virtio_gpu_driver : public virtio_pci_driver, public gfxfb::display {
public:
    virtio_gpu_driver(pci::device* dev)
        : virtio_pci_driver("virtio_gpu", "virtio-gpu", dev) {
        m_batch_lock.init();
    }

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

    int32_t flush(const gfxfb::gfxfb_rect* rects, uint32_t count) override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

private:
    static constexpr uint32_t MAX_BATCH_COMMANDS = 64;

    // The boot thread cannot sleep, so attach polls for completions
    enum class wait_mode : uint8_t { poll, sleep };

    int32_t allocate_command_page();
    bool batch_has_room(uint32_t bytes, uint32_t commands) const;
    int32_t queue_command(const void* request, uint32_t request_size,
                          uint32_t response_size, uint32_t* out_response_offset);
    int32_t run_batch(wait_mode mode);
    int32_t wait_for_batch(wait_mode mode);

    // For commands whose response is only a header
    template <typename T>
    int32_t queue_command(const T& request) {
        return queue_command(&request, sizeof(request), sizeof(virtio::virtio_gpu_ctrl_hdr), nullptr);
    }

    int32_t query_display(uint32_t* out_scanout, uint32_t* out_width, uint32_t* out_height);
    int32_t create_scanout_surface(uint32_t scanout, uint32_t width, uint32_t height);
    int32_t queue_rect_update(const gfxfb::gfxfb_rect& rect);

    virtio::virtqueue m_control_queue;
    uintptr_t m_control_notify = 0;

    // A batch's requests and responses share this DMA page
    uintptr_t m_command_page_va = 0;
    pmm::phys_addr_t m_command_page_phys = 0;
    uint32_t m_batch_bytes = 0;
    uint32_t m_batch_commands = 0;
    uint32_t m_response_offsets[MAX_BATCH_COMMANDS] = {};

    uintptr_t m_backing_va = 0;
    gfxfb::surface m_surface = {};

    // Held across a batch's completion wait
    sync::mutex m_batch_lock;
    bool m_has_completion_interrupt = false;
    bool m_is_registered = false;
    bool m_is_broken = false; // a batch timed out, so the device may still own the page
};

} // namespace drivers

#endif // STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H
