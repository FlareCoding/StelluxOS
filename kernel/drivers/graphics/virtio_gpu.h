#ifndef STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H
#define STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H

#include "drivers/virtio/virtio_pci_driver.h"
#include "drivers/graphics/gfxfb.h"
#include "common/list.h"
#include "dma/dma.h"
#include "sync/mutex.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "timer/timer.h"

namespace drivers::virtio {

constexpr uint16_t VIRTIO_DEV_GPU = VIRTIO_DEV_MODERN_BASE + 16;

constexpr uint16_t VIRTIO_GPU_QUEUE_CONTROL = 0;
constexpr uint16_t VIRTIO_GPU_QUEUE_CURSOR  = 1;

constexpr uint32_t VIRTIO_GPU_CMD_GET_DISPLAY_INFO        = 0x0100;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      = 0x0101;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_UNREF          = 0x0102;
constexpr uint32_t VIRTIO_GPU_CMD_SET_SCANOUT             = 0x0103;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_FLUSH          = 0x0104;
constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     = 0x0105;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107;

constexpr uint32_t VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300;
constexpr uint32_t VIRTIO_GPU_CMD_MOVE_CURSOR   = 0x0301;

constexpr uint32_t VIRTIO_GPU_RESP_OK_NODATA         = 0x1100;
constexpr uint32_t VIRTIO_GPU_RESP_OK_DISPLAY_INFO   = 0x1101;
constexpr uint32_t VIRTIO_GPU_RESP_ERR_UNSPEC        = 0x1200;
constexpr uint32_t VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY = 0x1201;

constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1;
constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2;

constexpr uint32_t VIRTIO_GPU_MAX_SCANOUTS = 16;

// The bit of the config region's `events_read` that reports a change of the scanouts' display info
constexpr uint32_t VIRTIO_GPU_EVENT_DISPLAY = 1;

// The device config region, not packed so that each field is read or written with one 32-bit access, as virtio asks
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

// The cursor picture's top left corner on a scanout, each coordinate a signed 32-bit value in an unsigned field
struct virtio_gpu_cursor_pos {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
} __attribute__((packed));

// Both cursor queue commands, which the device answers by handing them back with no response
struct virtio_gpu_update_cursor {
    virtio_gpu_ctrl_hdr   hdr;
    virtio_gpu_cursor_pos pos;
    uint32_t              resource_id; // 0 hides the cursor
    uint32_t              hot_x;
    uint32_t              hot_y;
    uint32_t              padding;
} __attribute__((packed));

} // namespace drivers::virtio

namespace drivers {

// A buffer whose RAM backs a resource, the device's own copy of the picture
struct virtio_gpu_buffer : gfxfb::buffer {
    dma::buffer backing;
    uint32_t    resource_id;
    list::node  release_link; // on the driver task's release list once gfxfb hands the buffer back
};

class virtio_gpu_driver;

// One of the device's scanouts as the driver drives it, in the driver's `m_scanouts`
struct virtio_gpu_scanout {
    // Written at attach and detach, otherwise only by the driver's task under both `m_batch_lock` and `m_scanout_lock`,
    // so the task reads them with no lock and the display calls hold one of the two
    virtio_gpu_buffer* base;          // the buffer the scanout was lit with, null while it is dark
    bool               is_relighting; // dark only while the task lights it again at another size

    // Read and written under `m_batch_lock` once the driver is attached
    uint64_t lit_ns;            // when the scanout was last lit, the first of its refresh boundaries
    uint32_t shown_resource_id; // the resource the scanout shows while lit

    // The cursor, read and written under `m_batch_lock`: the hotspot's last position in the scanout's pixels, and the
    // hotspot in the picture shown while `is_cursor_shown`
    int32_t  cursor_x;
    int32_t  cursor_y;
    uint32_t cursor_hot_x;
    uint32_t cursor_hot_y;
    bool     is_cursor_shown;

    // The latch of the last present the device confirmed, pending until the latch timer or the scanout going dark ends
    // that present. The timer guards its own state, and the fields after it are under `m_scanout_lock`
    timer::deadline_timer latch_timer;
    bool                  has_pending_latch;
    uint64_t              latch_frame;
    uint64_t              latch_ns;

    // Set at construction, for the latch timer's callback, which receives only its timer
    virtio_gpu_driver* driver;
    uint32_t           index;
};

/**
 * A virtio GPU used as a 2D display whose outputs are the device's scanouts
 * (its virtual monitors). Each buffer is RAM backing a resource, the device's
 * own copy of a picture. The driver lights every scanout the device enables at
 * a usable size (nonzero and at most 4096 by 4096), each with a base buffer of
 * its own, which userland draws into, and `flush()` copies the changed rects
 * from a base buffer's RAM into its resource. They reach the screen while the
 * scanout shows its base buffer. `create_buffer()` makes further buffers. Once
 * nothing refers to a created buffer, or to a base buffer whose scanout has
 * gone dark, gfxfb hands it back and `release_buffer()` queues it for the
 * driver's task, which takes the RAM back from the resource, drops the
 * resource and frees the RAM once the device confirms both, keeping the RAM
 * for good otherwise, since the device may still read it. `present()` copies a
 * buffer's regions, or the whole buffer when gfxfb passes none, into its
 * resource as `flush()` does and, once the device has confirmed every copy,
 * switches the scanout to that resource when it shows another. The device has
 * no vertical blank, so a deadline timer per scanout reports each present the
 * device confirmed latched at the scanout's next refresh boundary, within a
 * refresh period of `present()` returning, the boundaries falling 60 a second
 * from when the scanout was last lit. A present's batches share one deadline,
 * GFXFB_PRESENT_TIMEOUT_NS less a refresh period after the present takes the
 * driver's lock, but each counts as unanswered no sooner than a refresh period
 * after its submission, so `present()` can return past that deadline by up to
 * a refresh period for each batch that goes out within a refresh period of it
 * or later, as when its task waited for the CPU, instead of failing a device
 * that answers within a refresh period. Once a batch or a cursor command goes
 * unanswered, which the driver logs once, or the driver detaches, every call
 * that sends a command fails with ERR_DEVICE, but a present whose batch went
 * unanswered returns OK and ends with GFXFB_PRESENT_FAILED, since the device
 * may still read its buffer. Before that, a command the device refuses fails
 * its call with ERR_REFUSED, logged unless the device ran out of memory, or
 * with ERR_NO_MEMORY when the device has no memory left for a new buffer's
 * resource.
 *
 * The device reports a change of its scanouts through a configuration change
 * interrupt on a vector of its own, which wakes the driver's task. The task
 * reads the scanouts again, lights each dark one the device enables at a
 * usable size, takes each lit one it disables or reports at an unusable size
 * dark, and takes one it reports at another usable size dark with `relighting`
 * set before lighting it at that size, reporting each change to
 * `gfxfb::output_changed()`. Taking a scanout dark hides its cursor and has
 * the device stop showing any buffer on it before the scanout's unended
 * present, if any, ends with GFXFB_PRESENT_DARK. A scanout stays lit and
 * unreported when the device refuses either command or leaves it unanswered, a
 * dark one that cannot light stays dark, and one that cannot light at its new
 * size is reported dark without `relighting`. Each, like a failed read of the
 * scanouts, logs an error and waits for the device's next change, for good
 * once a command has gone unanswered, since every later read fails. If the
 * device hides the cursor and then refuses the other command, the scanout
 * stays lit with its cursor hidden until `set_cursor()` shows a picture again.
 * When the device enables no scanout at a usable size at attach, the driver
 * lights scanout 0 at 1024 by 768 anyway, and a change brings it to what the
 * device reports like any other. Without that interrupt, which the driver
 * logs, the scanouts stay as attach lit them. `read_output()` and
 * `base_buffer()` read the scanouts under a spinlock, so they never wait
 * behind a batch.
 *
 * Every scanout has a hardware cursor of up to 64 by 64 pixels, drawn from the
 * one cursor resource, which the driver shows on a lit scanout through
 * commands on the device's cursor queue. `set_cursor()` pads the picture with
 * transparent pixels into the resource's RAM, waits for the device to confirm
 * the copy into the resource, then sends the cursor command. A hide sends only
 * a cursor command naming no resource. `move_cursor()` sends the new position,
 * only recording it while the cursor is hidden. The device answers a cursor
 * command by handing it back with no response, so it can refuse only the
 * picture's copy. The cursor command is due GFXFB_PRESENT_TIMEOUT_NS after the
 * call takes the driver's lock and the copy a refresh period sooner, as a
 * present's batches are, but each of the two counts as unanswered no sooner
 * than a refresh period after its submission. So a cursor call returns within
 * GFXFB_PRESENT_TIMEOUT_NS of taking the lock, or a refresh period after its
 * last submission when its task waited for the CPU. Either way
 * `move_cursor()`, whose one submission follows the lock, returns within
 * GFXFB_PRESENT_TIMEOUT_NS of handing the device its command.
 */
class virtio_gpu_driver : public virtio_pci_driver, public gfxfb::display {
public:
    virtio_gpu_driver(pci::device* dev)
        : virtio_pci_driver("virtio_gpu", "virtio-gpu", dev) {
        m_batch_lock.init();
        m_scanout_lock = sync::SPINLOCK_INIT;
        m_work_lock = sync::SPINLOCK_INIT;
        m_releases.init();
        m_work_wq.init();

        for (uint32_t i = 0; i < virtio::VIRTIO_GPU_MAX_SCANOUTS; i++) {
            m_scanouts[i].driver = this;
            m_scanouts[i].index = i;
            timer::init_deadline_timer(&m_scanouts[i].latch_timer, report_latch);
        }
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
    int32_t present(
        uint32_t output,
        gfxfb::buffer* buf,
        const gfxfb::gfxfb_rect* regions,
        uint32_t region_count
    ) override;

    int32_t set_cursor(uint32_t output, const gfxfb::cursor_image& image) override;
    int32_t move_cursor(uint32_t output, int32_t x, int32_t y) override;

    void release_buffer(gfxfb::buffer* buf) override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

private:
    static constexpr uint32_t MAX_BATCH_COMMANDS = 64;

    // The boot thread cannot sleep, so attach polls for completions
    enum class wait_mode : uint8_t { poll, sleep };

    using release_list = list::head<virtio_gpu_buffer, &virtio_gpu_buffer::release_link>;

    // The work one pass of the task's loop takes: a configuration change the interrupt handler noted, the oldest buffer
    // gfxfb handed back, or both
    struct task_work {
        bool               has_config_change;
        virtio_gpu_buffer* released; // the oldest buffer gfxfb handed back, or null
    };

    int32_t allocate_command_page();
    bool batch_has_room(uint32_t bytes, uint32_t commands) const;
    int32_t queue_command(const void* request, uint32_t request_size,
                          uint32_t response_size, uint32_t* out_response_offset);
    int32_t run_batch(wait_mode mode);
    int32_t run_batch_until(wait_mode mode, uint64_t deadline_ns);
    int32_t wait_for_used(virtio::virtqueue& queue, uint32_t count, wait_mode mode, uint64_t deadline_ns);

    // For commands whose response is only a header
    template <typename T>
    int32_t queue_command(const T& request) {
        return queue_command(&request, sizeof(request), sizeof(virtio::virtio_gpu_ctrl_hdr), nullptr);
    }

    uint32_t read_scanout_count();
    int32_t read_display_info(wait_mode mode, virtio::virtio_gpu_display_one* out_modes);
    int32_t allocate_backing(virtio_gpu_buffer& buf, uint32_t width, uint32_t height, paging::page_flags_t access);
    int32_t create_resource(virtio_gpu_buffer& buf, uint32_t format, wait_mode mode);
    int32_t destroy_resource(const virtio_gpu_buffer& buf, wait_mode mode);
    int32_t queue_set_scanout(uint32_t scanout, const virtio_gpu_buffer& buf);
    void set_scanout_base(virtio_gpu_scanout& scanout, virtio_gpu_buffer* base, bool relighting);
    virtio_gpu_buffer* allocate_base(uint32_t index, uint32_t width, uint32_t height);
    int32_t light_scanout(uint32_t index, virtio_gpu_buffer& base, wait_mode mode);
    int32_t light_at_attach(uint32_t index, uint32_t width, uint32_t height);
    int32_t light_enabled_scanouts(const virtio::virtio_gpu_display_one* modes);
    int32_t darken_scanout(uint32_t index, bool relighting);
    void update_scanout(uint32_t index, const virtio::virtio_gpu_display_one& reported);
    void apply_config_change();
    int32_t create_cursor_resource();
    int32_t run_cursor_command(const virtio::virtio_gpu_update_cursor& command, uint64_t deadline_ns);
    int32_t update_cursor(virtio_gpu_scanout& scanout, const gfxfb::cursor_image& image, uint64_t deadline_ns);
    virtio::virtio_gpu_update_cursor cursor_command(
        const virtio_gpu_scanout& scanout,
        uint32_t type,
        uint32_t hot_x,
        uint32_t hot_y
    ) const;
    int32_t queue_rect_update(const virtio_gpu_buffer& buf, const gfxfb::gfxfb_rect& rect, uint64_t deadline_ns);
    int32_t show_buffer(
        virtio_gpu_scanout& scanout,
        const virtio_gpu_buffer& buf,
        const gfxfb::gfxfb_rect* regions,
        uint32_t count
    );
    void set_latch_timer(virtio_gpu_scanout& scanout);
    static void report_latch(timer::deadline_timer* latch_timer);
    task_work wait_for_work();

    virtio::virtqueue m_control_queue;
    uintptr_t m_control_notify = 0;
    virtio::virtqueue m_cursor_queue;
    uintptr_t m_cursor_notify = 0;

    // A batch's requests and responses share this DMA page, and a cursor command is staged at its start between batches
    dma::buffer m_command_page = {};
    uint32_t m_batch_bytes = 0;
    uint32_t m_batch_commands = 0;
    uint32_t m_response_offsets[MAX_BATCH_COMMANDS] = {};

    // Fixed at attach, so the display calls read it with no lock
    uint32_t m_scanout_count = 0;

    // Held across a batch's or a cursor command's completion wait
    sync::mutex m_batch_lock;
    bool m_has_completion_interrupt = false;
    bool m_is_registered = false;
    bool m_is_broken = false; // a batch or a cursor command timed out, so the device may still own the page

    // Read and advanced under `m_batch_lock` once the driver is attached
    uint32_t m_next_resource_id = 1;

    // The cursor resource, created at attach, whose RAM holds the last picture `set_cursor()` padded into it
    virtio_gpu_buffer m_cursor_buffer = {};

    // The first `m_scanout_count` are the device's. The latch timers' callbacks and the display calls that must not
    // wait take `m_scanout_lock`, so a holder must never keep it across a wait
    virtio_gpu_scanout m_scanouts[virtio::VIRTIO_GPU_MAX_SCANOUTS] = {};
    sync::spinlock     m_scanout_lock;

    // A base buffer the device may still read after a failed attach, which `detach()` frees once it resets the device
    virtio_gpu_buffer* m_held_base = nullptr;

    // The task's work: a configuration change the interrupt handler noted, and buffers gfxfb handed back, in order. The
    // interrupt handler and the reaper take the lock, so a holder must never keep it across a wait
    sync::spinlock   m_work_lock;
    bool             m_has_config_change = false;
    release_list     m_releases;
    sync::wait_queue m_work_wq;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_H
