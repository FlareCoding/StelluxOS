#ifndef STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H
#define STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H

#include "drivers/virtio/virtio_pci_driver.h"
#include "drivers/graphics/gfxfb.h"
#include "common/list.h"
#include "dma/dma.h"
#include "sync/mutex.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace drivers::virtio {

constexpr uint16_t VIRTIO_DEV_GPU = VIRTIO_DEV_MODERN_BASE + 16;

constexpr uint16_t VIRTIO_GPU_QUEUE_CONTROL = 0;

constexpr uint32_t VIRTIO_GPU_CMD_GET_DISPLAY_INFO        = 0x0100;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      = 0x0101;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_UNREF          = 0x0102;
constexpr uint32_t VIRTIO_GPU_CMD_SET_SCANOUT             = 0x0103;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_FLUSH          = 0x0104;
constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     = 0x0105;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107;

constexpr uint32_t VIRTIO_GPU_RESP_OK_NODATA         = 0x1100;
constexpr uint32_t VIRTIO_GPU_RESP_OK_DISPLAY_INFO   = 0x1101;
constexpr uint32_t VIRTIO_GPU_RESP_ERR_UNSPEC        = 0x1200;
constexpr uint32_t VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY = 0x1201;

constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2;

constexpr uint32_t VIRTIO_GPU_MAX_SCANOUTS = 16;

// The device config region, not packed so that each field is read with one 32-bit access, as virtio asks
struct virtio_gpu_config {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
};

static_assert(sizeof(virtio_gpu_config) == 16, "virtio's layout of the GPU config region");

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

struct virtio_gpu_resource_unref {
    virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
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

struct virtio_gpu_resource_detach_backing {
    virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
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

// A buffer whose RAM backs a resource, the device's own copy of the picture
struct virtio_gpu_buffer : gfxfb::buffer {
    dma::buffer backing;
    uint32_t    resource_id;
    list::node  release_link; // on the driver task's release list once gfxfb hands the buffer back
};

/**
 * A virtio GPU used as a 2D display whose outputs are the device's scanouts
 * (its virtual monitors). Each buffer is RAM backing a resource, the device's
 * own copy of a picture. The driver lights one scanout with its base buffer,
 * which userland draws into, and `flush()` copies the changed rects from the
 * RAM into the resource and shows them. `create_buffer()` makes further
 * buffers, and `release_buffer()` queues each for the driver's task, which
 * takes the RAM back from the resource, drops the resource and frees the RAM
 * once the device confirms both, keeping the RAM for good otherwise, since the
 * device may still read it. gfxfb keeps a registered display's base buffer for
 * the life of the kernel, so only created buffers come back. `present()`
 * returns ERR_UNSUPPORTED for every buffer, so the lit scanout always shows
 * its base buffer. Once a batch goes unanswered, which the driver logs once,
 * or the driver detaches, every call that sends a command fails with
 * ERR_DEVICE. Before that, a command the device refuses fails its call with
 * ERR_REFUSED, logged unless the device ran out of memory, or with
 * ERR_NO_MEMORY when the device has no memory left for a new buffer's
 * resource.
 */
class virtio_gpu_driver : public virtio_pci_driver, public gfxfb::display {
public:
    virtio_gpu_driver(pci::device* dev)
        : virtio_pci_driver("virtio_gpu", "virtio-gpu", dev) {
        m_batch_lock.init();
        m_release_lock = sync::SPINLOCK_INIT;
        m_releases.init();
        m_release_wq.init();
    }

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

    uint32_t output_count() const override { return m_scanout_count; }
    bool needs_flush() const override { return true; }
    int32_t read_output(uint32_t output, gfxfb::output_state& out) override;
    gfxfb::buffer* base_buffer(uint32_t output) override;
    int32_t flush(uint32_t output, const gfxfb::gfxfb_rect* rects, uint32_t count) override;
    int32_t create_buffer(uint32_t width, uint32_t height, uint32_t format, gfxfb::buffer** out) override;
    int32_t present(uint32_t, gfxfb::buffer*, const gfxfb::gfxfb_rect*, uint32_t) override;
    void release_buffer(gfxfb::buffer* buf) override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

private:
    static constexpr uint32_t MAX_BATCH_COMMANDS = 64;

    // The boot thread cannot sleep, so attach polls for completions
    enum class wait_mode : uint8_t { poll, sleep };

    using release_list = list::head<virtio_gpu_buffer, &virtio_gpu_buffer::release_link>;

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

    uint32_t read_scanout_count();
    int32_t query_display(uint32_t* out_scanout, uint32_t* out_width, uint32_t* out_height);
    int32_t allocate_backing(virtio_gpu_buffer& buf, uint32_t width, uint32_t height);
    int32_t create_resource(virtio_gpu_buffer& buf, wait_mode mode);
    int32_t destroy_resource(const virtio_gpu_buffer& buf);
    int32_t light_scanout(uint32_t scanout, uint32_t width, uint32_t height);
    int32_t queue_rect_update(const gfxfb::gfxfb_rect& rect);
    virtio_gpu_buffer* wait_for_release();

    virtio::virtqueue m_control_queue;
    uintptr_t m_control_notify = 0;

    // A batch's requests and responses share this DMA page
    dma::buffer m_command_page = {};
    uint32_t m_batch_bytes = 0;
    uint32_t m_batch_commands = 0;
    uint32_t m_response_offsets[MAX_BATCH_COMMANDS] = {};

    // Fixed at attach, so the display calls read them with no lock
    uint32_t          m_scanout_count = 0;
    uint32_t          m_scanout = 0; // the lit scanout
    virtio_gpu_buffer m_base_buffer = {};

    // Held across a batch's completion wait
    sync::mutex m_batch_lock;
    bool m_has_completion_interrupt = false;
    bool m_is_registered = false;
    bool m_is_broken = false; // a batch timed out, so the device may still own the page

    // Read and advanced under `m_batch_lock` once the driver is attached
    uint32_t m_next_resource_id = 1;

    // Buffers gfxfb handed back, taken off in order by the driver's task. The reaper takes the lock in
    // `release_buffer()`, so a holder must never keep it across a wait
    sync::spinlock   m_release_lock;
    release_list     m_releases;
    sync::wait_queue m_release_wq;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H
