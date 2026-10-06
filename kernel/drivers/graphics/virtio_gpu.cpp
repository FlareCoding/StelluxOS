#include "drivers/graphics/virtio_gpu.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "hw/cpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"

namespace drivers {

using namespace virtio;

constexpr int32_t ERR_DEVICE     = -1;
constexpr int32_t ERR_BATCH_FULL = -2;

constexpr uint32_t RESOURCE_ID = 1;

constexpr uint16_t BITS_PER_PIXEL = 32;
constexpr uint32_t BYTES_PER_PIXEL = BITS_PER_PIXEL / 8;

// A B8G8R8X8 pixel read as a little-endian 32-bit value
constexpr uint8_t RED_SHIFT = 16;
constexpr uint8_t GREEN_SHIFT = 8;
constexpr uint8_t BLUE_SHIFT = 0;

// Used when the device reports no usable scanout
constexpr uint32_t DEFAULT_WIDTH = 1024;
constexpr uint32_t DEFAULT_HEIGHT = 768;

// Bounds the backing allocation against a bad scanout size
constexpr uint32_t MAX_DIMENSION = 4096;

// A polled batch slower than this marks the device dead
constexpr uint64_t POLL_TIMEOUT_NS = 1000000000ULL;

// A request chained to a response
constexpr uint32_t DESCRIPTORS_PER_COMMAND = 2;

// A transfer and a flush
constexpr uint32_t COMMANDS_PER_RECT = 2;

// The alignment of the widest request or response field
constexpr uint32_t COMMAND_ALIGN = 8;

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

int32_t virtio_gpu_driver::allocate_command_page() {
    int32_t rc = 0;
    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(
            1, pmm::ZONE_DMA32,
            paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_USER | paging::PAGE_DMA,
            vmm::ALLOC_ZERO, kva::tag::generic,
            m_command_page_va, m_command_page_phys)
    );

    if (rc != vmm::OK) {
        log::error("virtio-gpu: failed to allocate the command page");
        return ERR_DEVICE;
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
        return ERR_DEVICE;
    }

    if (!batch_has_room(command_footprint(request_size, response_size), 1)) {
        return ERR_BATCH_FULL;
    }

    uint32_t request_offset = m_batch_bytes;
    uint32_t response_offset = request_offset + align_command_bytes(request_size);
    auto* page = reinterpret_cast<uint8_t*>(m_command_page_va);
    string::memcpy(page + request_offset, request, request_size);
    string::memset(page + response_offset, 0, response_size);

    int32_t head = m_control_queue.add_buf_chain(
        m_command_page_phys + request_offset, request_size,
        m_command_page_phys + response_offset, response_size,
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
    uint64_t deadline = clock::now_ns() + POLL_TIMEOUT_NS;

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

        // Wait queues have no timeout, so only polling can detect a dead device
        if (mode == wait_mode::sleep && m_has_completion_interrupt) {
            wait_for_event();
        } else if (clock::now_ns() > deadline) {
            return ERR_DEVICE;
        } else {
            cpu::relax();
        }
    }
}

int32_t virtio_gpu_driver::run_batch(wait_mode mode) {
    if (m_is_broken) {
        return ERR_DEVICE;
    }

    if (m_batch_commands == 0) {
        return OK;
    }

    m_control_queue.kick(m_control_notify);

    if (wait_for_batch(mode) != OK) {
        log::error("virtio-gpu: device stopped answering commands");
        m_is_broken = true;
        return ERR_DEVICE;
    }

    int32_t rc = OK;
    auto* page = reinterpret_cast<const uint8_t*>(m_command_page_va);

    for (uint32_t i = 0; i < m_batch_commands && rc == OK; i++) {
        auto* response = reinterpret_cast<const virtio_gpu_ctrl_hdr*>(page + m_response_offsets[i]);

        if (!is_ok_response(response->type)) {
            log::error("virtio-gpu: command failed with response 0x%x", response->type);
            rc = ERR_DEVICE;
        }
    }

    m_batch_commands = 0;
    m_batch_bytes = 0;

    return rc;
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
        m_command_page_va + response_offset);

    if (display_info->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        log::error("virtio-gpu: display info came back as response 0x%x", display_info->hdr.type);
        return ERR_DEVICE;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
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

int32_t virtio_gpu_driver::create_scanout_surface(uint32_t scanout, uint32_t width, uint32_t height) {
    uint64_t pitch = static_cast<uint64_t>(width) * BYTES_PER_PIXEL;
    uint64_t size = pitch * height;
    size_t pages = pmm::page_align_up(size) / pmm::PAGE_SIZE;

    pmm::phys_addr_t phys = 0;
    int32_t rc = 0;
    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(
            pages, pmm::ZONE_ANY, paging::PAGE_KERNEL_RW,
            vmm::ALLOC_ZERO, kva::tag::generic,
            m_backing_va, phys)
    );

    if (rc != vmm::OK) {
        log::error("virtio-gpu: failed to allocate a %lu byte backing", size);
        return ERR_DEVICE;
    }

    virtio_gpu_resource_create_2d create = {};
    create.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = RESOURCE_ID;
    create.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width       = width;
    create.height      = height;

    virtio_gpu_resource_attach_backing attach_backing = {};
    attach_backing.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach_backing.resource_id  = RESOURCE_ID;
    attach_backing.nr_entries   = 1;
    attach_backing.entry.addr   = phys;
    attach_backing.entry.length = static_cast<uint32_t>(size);

    virtio_gpu_set_scanout set_scanout = {};
    set_scanout.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    set_scanout.r           = {0, 0, width, height};
    set_scanout.scanout_id  = scanout;
    set_scanout.resource_id = RESOURCE_ID;

    if (queue_command(create) != OK ||
        queue_command(attach_backing) != OK ||
        queue_command(set_scanout) != OK ||
        run_batch(wait_mode::poll) != OK) {
        return ERR_DEVICE;
    }

    m_surface = {
        .phys        = phys,
        .width       = width,
        .height      = height,
        .pitch       = pitch,
        .bpp         = BITS_PER_PIXEL,
        .red_shift   = RED_SHIFT,
        .green_shift = GREEN_SHIFT,
        .blue_shift  = BLUE_SHIFT,
        .cacheable   = true,
    };

    return OK;
}

// Runs the batch first when the rect does not fit
int32_t virtio_gpu_driver::queue_rect_update(const gfxfb::gfxfb_rect& rect) {
    virtio_gpu_transfer_to_host_2d transfer = {};
    transfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r           = {rect.x, rect.y, rect.width, rect.height};
    transfer.offset      = rect.y * m_surface.pitch + static_cast<uint64_t>(rect.x) * BYTES_PER_PIXEL;
    transfer.resource_id = RESOURCE_ID;

    virtio_gpu_resource_flush show = {};
    show.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    show.r           = transfer.r;
    show.resource_id = RESOURCE_ID;

    // Checked together so a rect is never copied without being shown
    uint32_t bytes = command_footprint(sizeof(transfer), sizeof(virtio_gpu_ctrl_hdr)) +
                     command_footprint(sizeof(show), sizeof(virtio_gpu_ctrl_hdr));

    if (!batch_has_room(bytes, COMMANDS_PER_RECT) && run_batch(wait_mode::sleep) != OK) {
        return ERR_DEVICE;
    }

    if (queue_command(transfer) != OK || queue_command(show) != OK) {
        return ERR_DEVICE;
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
        return ERR_DEVICE;
    }

    // Without MSI-X every batch is polled
    if (setup_msix(1) == OK) {
        m_has_completion_interrupt = route_queue_msix(VIRTIO_GPU_QUEUE_CONTROL, 0) == OK;
    }

    if (!m_has_completion_interrupt) {
        log::warn("virtio-gpu: no completion interrupt, polling instead");
    }

    set_driver_ok();

    uint32_t scanout = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    if (query_display(&scanout, &width, &height) != OK ||
        create_scanout_surface(scanout, width, height) != OK) {
        log::error("virtio-gpu: display setup failed");
        set_failed();
        return ERR_DEVICE;
    }

    RUN_ELEVATED(rc = gfxfb::register_display(m_surface, this));
    if (rc != gfxfb::OK) {
        log::error("virtio-gpu: display registration failed");
        set_failed();
        return ERR_DEVICE;
    }

    m_is_registered = true;
    log::info("virtio-gpu: %ux%u on scanout %u, device is live", width, height, scanout);

    return OK;
}

int32_t virtio_gpu_driver::detach() {
    // gfxfb can still call flush(), which must fail once the page is gone
    m_is_broken = true;
    reset_device();

    // gfxfb keeps a registered surface for the life of the kernel
    if (!m_is_registered && m_backing_va) {
        RUN_ELEVATED(vmm::free(m_backing_va));
        m_backing_va = 0;
    }

    if (m_command_page_va) {
        RUN_ELEVATED(vmm::free(m_command_page_va));
        m_command_page_va = 0;
    }

    return pci_driver::detach();
}

void virtio_gpu_driver::run() {
    // Batches complete in the flushing task, so this task has no work
}

int32_t virtio_gpu_driver::flush(const gfxfb::gfxfb_rect* rects, uint32_t count) {
    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));

    int32_t rc = OK;
    for (uint32_t i = 0; i < count && rc == OK; i++) {
        rc = queue_rect_update(rects[i]);
    }

    if (rc == OK) {
        rc = run_batch(wait_mode::sleep);
    }

    RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

    return rc == OK ? gfxfb::OK : gfxfb::ERR;
}

__PRIVILEGED_CODE void virtio_gpu_driver::on_interrupt(uint32_t vector) {
    (void)vector;
    ack_interrupt();
}

REGISTER_PCI_DRIVER(virtio_gpu_driver,
    PCI_MATCH(VIRTIO_VENDOR_ID, VIRTIO_DEV_GPU, PCI_MATCH_ANY_8, PCI_MATCH_ANY_8, PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(virtio_gpu_driver));

} // namespace drivers
