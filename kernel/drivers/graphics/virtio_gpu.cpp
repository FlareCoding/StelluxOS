#include "drivers/graphics/virtio_gpu.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "hw/cpu.h"
#include "mm/heap.h"

namespace drivers {

using namespace virtio;

constexpr int32_t ERR_IO         = -1;
constexpr int32_t ERR_BATCH_FULL = -2;

// The device left unanswered the command attaching a buffer's RAM to its resource, so the RAM must stay allocated
// unless a reset stops the device
constexpr int32_t ERR_ATTACH_UNANSWERED = -3;

// The device had no memory left for a command. A client's requests for buffers reach that limit on a healthy device
constexpr int32_t ERR_OUT_OF_DEVICE_MEMORY = -4;

// A B8G8R8X8 pixel, which a driver display's buffers hold as a little-endian 32-bit value
constexpr uint32_t BYTES_PER_PIXEL = 4;

// Used when the device reports no usable scanout
constexpr uint32_t DEFAULT_WIDTH = 1024;
constexpr uint32_t DEFAULT_HEIGHT = 768;

// The display info carries no refresh rate, so the driver supplies 60 Hz, a common monitor's rate, for the lit scanout
constexpr uint64_t REFRESH_MILLIHERTZ = 60000;

// Bounds a buffer's RAM against a bad scanout size or a client's request
constexpr uint32_t MAX_DIMENSION = 4096;

// A batch still unanswered this long after its submission marks the device dead
constexpr uint64_t BATCH_TIMEOUT_NS = 1000000000ULL;

// A request chained to a response
constexpr uint32_t DESCRIPTORS_PER_COMMAND = 2;

// A transfer and a flush
constexpr uint32_t COMMANDS_PER_RECT = 2;

// The alignment of the widest request or response field
constexpr uint32_t COMMAND_ALIGN = 8;

// The command page stays below 4 GiB with the virtqueue rings. A buffer's RAM, up to 64 MiB, may use any address,
// leaving DMA32 to devices that reach only 32 bits.
constexpr uint8_t COMMAND_PAGE_ADDRESS_BITS = 32;
constexpr uint8_t BACKING_ADDRESS_BITS = 64;

static uint32_t align_command_bytes(uint32_t size) {
    return (size + COMMAND_ALIGN - 1) & ~(COMMAND_ALIGN - 1);
}

// Page bytes taken by a request and its response
static uint32_t command_footprint(uint32_t request_size, uint32_t response_size) {
    return align_command_bytes(request_size) + align_command_bytes(response_size);
}

// OK responses sit below the first error type
static bool is_ok_response(uint32_t type) {
    return type >= VIRTIO_GPU_RESP_OK_NODATA && type < VIRTIO_GPU_RESP_ERR_UNSPEC;
}

static bool is_usable_mode(const virtio_gpu_display_one& mode) {
    return mode.enabled && mode.r.width > 0 && mode.r.height > 0 &&
           mode.r.width <= MAX_DIMENSION && mode.r.height <= MAX_DIMENSION;
}

// The caller must then keep the buffer's RAM allocated for the life of the kernel
static void log_kept_ram(const virtio_gpu_buffer& buf) {
    log::error(
        "virtio-gpu: kept the %lu bytes of RAM backing resource %u for good, since the device may still read them",
        buf.size,
        buf.resource_id
    );
}

int32_t virtio_gpu_driver::allocate_command_page() {
    int32_t rc = 0;
    RUN_ELEVATED(
        rc = dma::alloc_pages(
            dma_profile(COMMAND_PAGE_ADDRESS_BITS),
            1,
            dma::caching::uncached,
            paging::PAGE_USER,
            m_command_page
        )
    );

    if (rc != dma::OK) {
        log::error("virtio-gpu: failed to allocate the command page");
        return ERR_IO;
    }

    return OK;
}

bool virtio_gpu_driver::batch_has_room(uint32_t bytes, uint32_t commands) const {
    return m_batch_bytes + bytes <= pmm::PAGE_SIZE &&
           m_batch_commands + commands <= MAX_BATCH_COMMANDS &&
           m_control_queue.free_count() >= commands * DESCRIPTORS_PER_COMMAND;
}

int32_t virtio_gpu_driver::queue_command(const void* request, uint32_t request_size,
                                         uint32_t response_size, uint32_t* out_response_offset) {
    if (m_is_broken) {
        return ERR_IO;
    }

    if (!batch_has_room(command_footprint(request_size, response_size), 1)) {
        return ERR_BATCH_FULL;
    }

    uint32_t request_offset = m_batch_bytes;
    uint32_t response_offset = request_offset + align_command_bytes(request_size);
    auto* page = reinterpret_cast<uint8_t*>(m_command_page.virt);
    string::memcpy(page + request_offset, request, request_size);
    string::memset(page + response_offset, 0, response_size);

    int32_t head = m_control_queue.add_buf_chain(
        m_command_page.phys + request_offset, request_size,
        m_command_page.phys + response_offset, response_size,
        0, VRING_DESC_F_WRITE);

    if (head < 0) {
        return ERR_BATCH_FULL;
    }

    m_response_offsets[m_batch_commands++] = response_offset;
    m_batch_bytes = response_offset + align_command_bytes(response_size);

    if (out_response_offset) {
        *out_response_offset = response_offset;
    }

    return OK;
}

int32_t virtio_gpu_driver::wait_for_batch(wait_mode mode) {
    uint32_t completed = 0;
    uint64_t deadline = clock::now_ns() + BATCH_TIMEOUT_NS;

    while (true) {
        uint16_t head = 0;
        uint32_t written = 0;

        while (m_control_queue.get_used(&head, &written)) {
            m_control_queue.free_desc(head);
            completed++;
        }

        if (completed >= m_batch_commands) {
            return OK;
        }

        if (clock::now_ns() >= deadline) {
            return ERR_IO;
        }

        if (mode == wait_mode::sleep && m_has_completion_interrupt) {
            wait_for_event_until(deadline);
        } else {
            cpu::relax();
        }
    }
}

int32_t virtio_gpu_driver::run_batch(wait_mode mode) {
    if (m_is_broken) {
        return ERR_IO;
    }

    if (m_batch_commands == 0) {
        return OK;
    }

    m_control_queue.kick(m_control_notify);

    if (wait_for_batch(mode) != OK) {
        log::error("virtio-gpu: device stopped answering commands");
        m_is_broken = true;
        return ERR_IO;
    }

    int32_t rc = OK;
    auto* page = reinterpret_cast<const uint8_t*>(m_command_page.virt);

    for (uint32_t i = 0; i < m_batch_commands && rc == OK; i++) {
        auto* response = reinterpret_cast<const virtio_gpu_ctrl_hdr*>(page + m_response_offsets[i]);

        if (response->type == VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY) {
            rc = ERR_OUT_OF_DEVICE_MEMORY;
        } else if (!is_ok_response(response->type)) {
            log::error("virtio-gpu: command failed with response 0x%x", response->type);
            rc = ERR_IO;
        }
    }

    m_batch_commands = 0;
    m_batch_bytes = 0;

    return rc;
}

// The device's scanout count, at least 1 so the fallback scanout counts, and at most the display info's 16 entries
uint32_t virtio_gpu_driver::read_scanout_count() {
    volatile virtio_gpu_config* cfg = device_config<virtio_gpu_config>();
    uint32_t count = cfg ? cfg->num_scanouts : 1;

    if (count == 0) {
        return 1;
    }

    return count < VIRTIO_GPU_MAX_SCANOUTS ? count : VIRTIO_GPU_MAX_SCANOUTS;
}

int32_t virtio_gpu_driver::query_display(uint32_t* out_scanout, uint32_t* out_width, uint32_t* out_height) {
    virtio_gpu_ctrl_hdr request = {};
    request.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    uint32_t response_offset = 0;
    int32_t rc = queue_command(&request, sizeof(request),
                               sizeof(virtio_gpu_resp_display_info), &response_offset);

    if (rc != OK) {
        return rc;
    }

    rc = run_batch(wait_mode::poll);
    if (rc != OK) {
        return rc;
    }

    // The response stays in the page until the next command is queued
    auto* display_info = reinterpret_cast<const virtio_gpu_resp_display_info*>(
        m_command_page.virt + response_offset);

    if (display_info->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        log::error("virtio-gpu: display info came back as response 0x%x", display_info->hdr.type);
        return ERR_IO;
    }

    for (uint32_t i = 0; i < m_scanout_count; i++) {
        virtio_gpu_display_one mode = display_info->pmodes[i];

        if (is_usable_mode(mode)) {
            *out_scanout = i;
            *out_width = mode.r.width;
            *out_height = mode.r.height;
            return OK;
        }
    }

    log::warn("virtio-gpu: no usable scanout reported, using %ux%u", DEFAULT_WIDTH, DEFAULT_HEIGHT);
    *out_scanout = 0;
    *out_width = DEFAULT_WIDTH;
    *out_height = DEFAULT_HEIGHT;

    return OK;
}

// Allocates the RAM for a width by height picture and fills the buffer's layout from it
int32_t virtio_gpu_driver::allocate_backing(virtio_gpu_buffer& buf, uint32_t width, uint32_t height) {
    uint64_t pitch = static_cast<uint64_t>(width) * BYTES_PER_PIXEL;
    size_t pages = pmm::page_align_up(pitch * height) / pmm::PAGE_SIZE;

    int32_t rc = 0;
    RUN_ELEVATED(
        rc = dma::alloc_pages(
            dma_profile(BACKING_ADDRESS_BITS),
            pages,
            dma::caching::cached_when_coherent,
            0,
            buf.backing
        )
    );

    if (rc != dma::OK) {
        return ERR_IO;
    }

    buf.width     = width;
    buf.height    = height;
    buf.pitch     = static_cast<uint32_t>(pitch);
    buf.size      = pages * pmm::PAGE_SIZE;
    buf.phys      = buf.backing.phys;
    buf.cacheable = buf.backing.cached;

    return OK;
}

// Runs under `m_batch_lock` once the driver is attached. On any failure but `ERR_ATTACH_UNANSWERED` the device holds
// none of the RAM
int32_t virtio_gpu_driver::create_resource(virtio_gpu_buffer& buf, wait_mode mode) {
    buf.resource_id = m_next_resource_id++;

    virtio_gpu_resource_create_2d create = {};
    create.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = buf.resource_id;
    create.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width       = buf.width;
    create.height      = buf.height;

    virtio_gpu_resource_attach_backing attach_backing = {};
    attach_backing.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach_backing.resource_id  = buf.resource_id;
    attach_backing.nr_entries   = 1;
    attach_backing.entry.addr   = buf.phys;
    attach_backing.entry.length = buf.pitch * buf.height;

    virtio_gpu_resource_unref unref = {};
    unref.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.resource_id = buf.resource_id;

    if (queue_command(create) != OK) {
        return ERR_IO;
    }

    int32_t rc = run_batch(mode);
    if (rc != OK) {
        return rc;
    }

    if (queue_command(attach_backing) == OK && run_batch(mode) == OK) {
        return OK;
    }

    if (m_is_broken) {
        return ERR_ATTACH_UNANSWERED;
    }

    // The device refused the RAM, so the resource it created is dropped
    if (queue_command(unref) == OK) {
        run_batch(mode);
    }

    return ERR_IO;
}

// Runs under `m_batch_lock`. Takes the RAM back from the buffer's resource and drops the resource
int32_t virtio_gpu_driver::destroy_resource(const virtio_gpu_buffer& buf) {
    virtio_gpu_resource_detach_backing detach_backing = {};
    detach_backing.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    detach_backing.resource_id = buf.resource_id;

    virtio_gpu_resource_unref unref = {};
    unref.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.resource_id = buf.resource_id;

    if (queue_command(detach_backing) != OK ||
        queue_command(unref) != OK ||
        run_batch(wait_mode::sleep) != OK) {
        return ERR_IO;
    }

    return OK;
}

int32_t virtio_gpu_driver::light_scanout(uint32_t scanout, uint32_t width, uint32_t height) {
    if (allocate_backing(m_base_buffer, width, height) != OK) {
        log::error("virtio-gpu: failed to allocate the RAM for a %ux%u scanout", width, height);
        return ERR_IO;
    }

    if (create_resource(m_base_buffer, wait_mode::poll) != OK) {
        return ERR_IO;
    }

    virtio_gpu_set_scanout set_scanout = {};
    set_scanout.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    set_scanout.r           = {0, 0, width, height};
    set_scanout.scanout_id  = scanout;
    set_scanout.resource_id = m_base_buffer.resource_id;

    if (queue_command(set_scanout) != OK || run_batch(wait_mode::poll) != OK) {
        return ERR_IO;
    }

    m_scanout = scanout;

    return OK;
}

// Runs the batch first when the rect does not fit
int32_t virtio_gpu_driver::queue_rect_update(const gfxfb::gfxfb_rect& rect) {
    uint64_t row_offset = static_cast<uint64_t>(rect.y) * m_base_buffer.pitch;

    virtio_gpu_transfer_to_host_2d transfer = {};
    transfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r           = {rect.x, rect.y, rect.width, rect.height};
    transfer.offset      = row_offset + static_cast<uint64_t>(rect.x) * BYTES_PER_PIXEL;
    transfer.resource_id = m_base_buffer.resource_id;

    virtio_gpu_resource_flush show = {};
    show.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    show.r           = transfer.r;
    show.resource_id = m_base_buffer.resource_id;

    // Checked together so a rect is never copied without being shown
    uint32_t bytes = command_footprint(sizeof(transfer), sizeof(virtio_gpu_ctrl_hdr)) +
                     command_footprint(sizeof(show), sizeof(virtio_gpu_ctrl_hdr));

    if (!batch_has_room(bytes, COMMANDS_PER_RECT) && run_batch(wait_mode::sleep) != OK) {
        return ERR_IO;
    }

    if (queue_command(transfer) != OK || queue_command(show) != OK) {
        return ERR_IO;
    }

    return OK;
}

int32_t virtio_gpu_driver::attach() {
    log::info("virtio-gpu: attaching to %02x:%02x.%x",
              m_dev->bus(), m_dev->slot(), m_dev->func());

    uint64_t features = 0;
    int32_t rc = start_transport(VIRTIO_F_VERSION_1, 0, &features);
    if (rc != OK) {
        return rc;
    }

    if (setup_queue(VIRTIO_GPU_QUEUE_CONTROL, m_control_queue, &m_control_notify) != OK ||
        allocate_command_page() != OK) {
        set_failed();
        return ERR_IO;
    }

    // Without MSI-X every batch is polled
    if (setup_msix(1) == OK) {
        m_has_completion_interrupt = route_queue_msix(VIRTIO_GPU_QUEUE_CONTROL, 0) == OK;
    }

    if (!m_has_completion_interrupt) {
        log::warn("virtio-gpu: no completion interrupt, polling instead");
    }

    set_driver_ok();

    m_scanout_count = read_scanout_count();

    uint32_t scanout = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    if (query_display(&scanout, &width, &height) != OK ||
        light_scanout(scanout, width, height) != OK) {
        log::error("virtio-gpu: display setup failed");
        set_failed();
        return ERR_IO;
    }

    RUN_ELEVATED(rc = gfxfb::register_display(this));
    if (rc != gfxfb::OK) {
        log::error("virtio-gpu: display registration failed");
        set_failed();
        return ERR_IO;
    }

    m_is_registered = true;
    log::info("virtio-gpu: %ux%u on scanout %u of %u, device is live", width, height, scanout, m_scanout_count);

    return OK;
}

int32_t virtio_gpu_driver::detach() {
    // gfxfb can still call flush(), which must fail once the page is gone
    m_is_broken = true;
    reset_device();

    // gfxfb keeps a registered display's base buffer for the life of the kernel
    if (!m_is_registered) {
        RUN_ELEVATED(dma::free_pages(m_base_buffer.backing));
    }

    RUN_ELEVATED(dma::free_pages(m_command_page));

    return pci_driver::detach();
}

// Sleeps until gfxfb has handed a buffer back, then takes the oldest off the list
virtio_gpu_buffer* virtio_gpu_driver::wait_for_release() {
    virtio_gpu_buffer* buf = nullptr;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_release_lock);
        while (m_releases.empty()) {
            irq = sync::wait(m_release_wq, m_release_lock, irq);
        }

        buf = m_releases.pop_front();
        sync::spin_unlock_irqrestore(m_release_lock, irq);
    });

    return buf;
}

// Releases buffers gfxfb handed back, since the release waits for the device and the reaper must not
void virtio_gpu_driver::run() {
    while (true) {
        virtio_gpu_buffer* buf = wait_for_release();

        RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
        int32_t rc = destroy_resource(*buf);
        RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

        // Unless the device confirmed both commands, the resource may still hold the RAM
        if (rc != OK) {
            log_kept_ram(*buf);
            continue;
        }

        RUN_ELEVATED(dma::free_pages(buf->backing));
        heap::ufree_delete(buf);
    }
}

int32_t virtio_gpu_driver::create_buffer(uint32_t width, uint32_t height, uint32_t format, gfxfb::buffer** out) {
    bool is_supported_format = format == gfxfb::GFXFB_FORMAT_XRGB8888;
    bool fits = width <= MAX_DIMENSION && height <= MAX_DIMENSION;
    if (!is_supported_format || !fits) {
        return gfxfb::ERR_INVALID_ARG;
    }

    auto* buf = heap::ualloc_new<virtio_gpu_buffer>();
    if (!buf) {
        return gfxfb::ERR_NO_MEMORY;
    }

    if (allocate_backing(*buf, width, height) != OK) {
        heap::ufree_delete(buf);
        return gfxfb::ERR_NO_MEMORY;
    }

    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
    int32_t rc = create_resource(*buf, wait_mode::sleep);
    bool is_broken = m_is_broken;
    RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

    if (rc == OK) {
        *out = buf;
        return gfxfb::OK;
    }

    if (rc == ERR_ATTACH_UNANSWERED) {
        log_kept_ram(*buf);
        return gfxfb::ERR_DEVICE;
    }

    RUN_ELEVATED(dma::free_pages(buf->backing));
    heap::ufree_delete(buf);

    if (rc == ERR_OUT_OF_DEVICE_MEMORY) {
        return gfxfb::ERR_NO_MEMORY;
    }

    // A device still answering refused the command, as it refuses a resource ID still in use once the IDs wrap
    return is_broken ? gfxfb::ERR_DEVICE : gfxfb::ERR_REFUSED;
}

int32_t virtio_gpu_driver::present(uint32_t, gfxfb::buffer*, const gfxfb::gfxfb_rect*, uint32_t) {
    return gfxfb::ERR_UNSUPPORTED;
}

void virtio_gpu_driver::release_buffer(gfxfb::buffer* buf) {
    auto* released = static_cast<virtio_gpu_buffer*>(buf);

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_release_lock);
        m_releases.push_back(released);
        sync::wake_one(m_release_wq);
        sync::spin_unlock_irqrestore(m_release_lock, irq);
    });
}

int32_t virtio_gpu_driver::read_output(uint32_t output, gfxfb::output_state& out) {
    if (output >= m_scanout_count) {
        return gfxfb::ERR_INVALID_ARG;
    }

    // The device does not say which scanout the boot firmware lit
    out.lit = output == m_scanout;
    out.boot_output = false;

    if (out.lit) {
        out.current = {
            .width              = m_base_buffer.width,
            .height             = m_base_buffer.height,
            .refresh_millihertz = REFRESH_MILLIHERTZ,
        };
    }

    return gfxfb::OK;
}

gfxfb::buffer* virtio_gpu_driver::base_buffer(uint32_t output) {
    return output == m_scanout ? &m_base_buffer : nullptr;
}

int32_t virtio_gpu_driver::flush(uint32_t output, const gfxfb::gfxfb_rect* rects, uint32_t count) {
    if (output != m_scanout) {
        return gfxfb::ERR_DARK;
    }

    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));

    int32_t rc = OK;
    for (uint32_t i = 0; i < count && rc == OK; i++) {
        rc = queue_rect_update(rects[i]);
    }

    if (rc == OK) {
        rc = run_batch(wait_mode::sleep);
    }

    bool is_broken = m_is_broken;
    RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

    if (rc == OK) {
        return gfxfb::OK;
    }

    return is_broken ? gfxfb::ERR_DEVICE : gfxfb::ERR_REFUSED;
}

__PRIVILEGED_CODE void virtio_gpu_driver::on_interrupt(uint32_t vector) {
    (void)vector;
    ack_interrupt();
}

REGISTER_PCI_DRIVER(virtio_gpu_driver,
    PCI_MATCH(VIRTIO_VENDOR_ID, VIRTIO_DEV_GPU, PCI_MATCH_ANY_8, PCI_MATCH_ANY_8, PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(virtio_gpu_driver));

} // namespace drivers
