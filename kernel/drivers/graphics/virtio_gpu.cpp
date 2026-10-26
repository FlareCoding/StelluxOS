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

// The device may still read a buffer's RAM after a failed lighting, so the RAM must stay allocated unless a reset stops
// the device
constexpr int32_t ERR_RAM_HELD = -5;

// A B8G8R8X8 pixel, which a driver display's buffers hold as a little-endian 32-bit value, or a cursor's B8G8R8A8 pixel
constexpr uint32_t BYTES_PER_PIXEL = 4;

// Used when the device reports no usable scanout at attach
constexpr uint32_t DEFAULT_WIDTH = 1024;
constexpr uint32_t DEFAULT_HEIGHT = 768;

// The display info carries no refresh rate, so the driver paces presents at 60 Hz, a common monitor's rate, and reports
// that rate for every lit scanout
constexpr uint64_t REFRESH_MILLIHERTZ = 60000;

// The time between two of a lit scanout's refresh boundaries, at which presents latch
constexpr uint64_t REFRESH_PERIOD_NS = 1000000000000ULL / REFRESH_MILLIHERTZ;

// Bounds a buffer's RAM against a bad scanout size or a client's request
constexpr uint32_t MAX_DIMENSION = 4096;

// The device draws a cursor only from a resource of exactly this width and height
constexpr uint32_t CURSOR_DIMENSION = 64;
static_assert(CURSOR_DIMENSION <= gfxfb::CURSOR_DIMENSION_MAX, "a cursor maximum gfxfb accepts");

// A batch still unanswered this long after its submission marks the device dead
constexpr uint64_t BATCH_TIMEOUT_NS = 1000000000ULL;

// The deadline of a present's batches, this long after the present takes the lock, so a present the device answers by
// then ends within GFXFB_PRESENT_TIMEOUT_NS even counted from the lock, its latch coming at most a refresh period later
constexpr uint64_t PRESENT_BATCH_TIMEOUT_NS = gfxfb::GFXFB_PRESENT_TIMEOUT_NS - REFRESH_PERIOD_NS;

// A batch or a cursor command counts as unanswered no sooner than this long after its submission, even past its
// deadline, so a task that waited for the CPU before submitting fails no device that answers within this long
constexpr uint64_t MIN_ANSWER_WAIT_NS = REFRESH_PERIOD_NS;

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

// Both queues complete on the first MSI-X vector, and configuration changes arrive on the second
constexpr uint16_t COMPLETION_VECTOR = 0;
constexpr uint16_t CONFIG_VECTOR = 1;
constexpr uint32_t MSIX_VECTORS = 2;

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

// `rect` cut to `buf`'s pixels, with a width and height of 0 when it lies wholly outside them
static gfxfb::gfxfb_rect clip_to_buffer(const gfxfb::gfxfb_rect& rect, const virtio_gpu_buffer& buf) {
    uint64_t right = static_cast<uint64_t>(rect.x) + rect.width;
    uint64_t bottom = static_cast<uint64_t>(rect.y) + rect.height;
    uint64_t clipped_right = right < buf.width ? right : buf.width;
    uint64_t clipped_bottom = bottom < buf.height ? bottom : buf.height;

    gfxfb::gfxfb_rect clipped = {rect.x, rect.y, 0, 0};
    if (rect.x < clipped_right && rect.y < clipped_bottom) {
        clipped.width = static_cast<uint32_t>(clipped_right - rect.x);
        clipped.height = static_cast<uint32_t>(clipped_bottom - rect.y);
    }

    return clipped;
}

// The device places a cursor by its picture's top left corner. A corner CURSOR_DIMENSION or more past the scanout's
// left or top edge shows nothing, so it is held there, which also keeps it a signed 32-bit value for the device
static uint32_t cursor_corner(int32_t hotspot_position, uint32_t hotspot) {
    int64_t corner = static_cast<int64_t>(hotspot_position) - hotspot;
    int64_t farthest = -static_cast<int64_t>(CURSOR_DIMENSION);
    if (corner < farthest) {
        corner = farthest;
    }

    return static_cast<uint32_t>(corner);
}

// When a request submitted just now counts as unanswered: at `deadline_ns`, or MIN_ANSWER_WAIT_NS from now when later
static uint64_t answer_deadline(uint64_t deadline_ns) {
    uint64_t earliest_failure_ns = clock::now_ns() + MIN_ANSWER_WAIT_NS;
    return deadline_ns > earliest_failure_ns ? deadline_ns : earliest_failure_ns;
}

// The caller must then keep the buffer's RAM allocated for the life of the kernel
static void log_kept_ram(const virtio_gpu_buffer& buf) {
    log::error(
        "virtio-gpu: kept the %lu bytes of RAM backing resource %u for good, since the device may still read them",
        buf.size,
        buf.resource_id
    );
}

// Frees a buffer from `ualloc_new`. The caller must call it only once the device holds none of the buffer's RAM
static void free_buffer(virtio_gpu_buffer* buf) {
    RUN_ELEVATED(dma::free_pages(buf->backing));
    heap::ufree_delete(buf);
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

// Waits until the device hands back `count` requests on `queue`, freeing their descriptors, failing at `deadline_ns`
int32_t virtio_gpu_driver::wait_for_used(virtqueue& queue, uint32_t count, wait_mode mode, uint64_t deadline_ns) {
    uint32_t completed = 0;

    while (true) {
        uint16_t head = 0;
        uint32_t written = 0;

        while (queue.get_used(&head, &written)) {
            queue.free_desc(head);
            completed++;
        }

        if (completed >= count) {
            return OK;
        }

        if (clock::now_ns() >= deadline_ns) {
            return ERR_IO;
        }

        if (mode == wait_mode::sleep && m_has_completion_interrupt) {
            wait_for_event_until(deadline_ns);
        } else {
            cpu::relax();
        }
    }
}

// Runs the batch with a deadline BATCH_TIMEOUT_NS after its submission
int32_t virtio_gpu_driver::run_batch(wait_mode mode) {
    return run_batch_until(mode, clock::now_ns() + BATCH_TIMEOUT_NS);
}

int32_t virtio_gpu_driver::run_batch_until(wait_mode mode, uint64_t deadline_ns) {
    if (m_is_broken) {
        return ERR_IO;
    }

    if (m_batch_commands == 0) {
        return OK;
    }

    m_control_queue.kick(m_control_notify);

    if (wait_for_used(m_control_queue, m_batch_commands, mode, answer_deadline(deadline_ns)) != OK) {
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

// Runs under `m_batch_lock` once the driver is attached. Fills `out_modes` with the display info of each scanout
int32_t virtio_gpu_driver::read_display_info(wait_mode mode, virtio_gpu_display_one* out_modes) {
    virtio_gpu_ctrl_hdr request = {};
    request.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    uint32_t response_offset = 0;
    int32_t rc = queue_command(&request, sizeof(request),
                               sizeof(virtio_gpu_resp_display_info), &response_offset);

    if (rc != OK) {
        return rc;
    }

    rc = run_batch(mode);
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
        out_modes[i] = display_info->pmodes[i];
    }

    return OK;
}

// Allocates the RAM for a width by height picture and fills the buffer's layout from it. `access` is
// `paging::PAGE_USER` for RAM the driver writes while lowered, and 0 otherwise
int32_t virtio_gpu_driver::allocate_backing(
    virtio_gpu_buffer& buf,
    uint32_t width,
    uint32_t height,
    paging::page_flags_t access
) {
    uint64_t pitch = static_cast<uint64_t>(width) * BYTES_PER_PIXEL;
    size_t pages = pmm::page_align_up(pitch * height) / pmm::PAGE_SIZE;

    int32_t rc = 0;
    RUN_ELEVATED(
        rc = dma::alloc_pages(
            dma_profile(BACKING_ADDRESS_BITS),
            pages,
            dma::caching::cached_when_coherent,
            access,
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

// Runs under `m_batch_lock` once the driver is attached. Gives the device a resource of `format` pixels backed by the
// buffer's RAM. On any failure but `ERR_ATTACH_UNANSWERED` the device holds none of the RAM
int32_t virtio_gpu_driver::create_resource(virtio_gpu_buffer& buf, uint32_t format, wait_mode mode) {
    buf.resource_id = m_next_resource_id++;

    virtio_gpu_resource_create_2d create = {};
    create.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = buf.resource_id;
    create.format      = format;
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

// Runs under `m_batch_lock` once the driver is attached. Takes the RAM back from the buffer's resource and drops the
// resource
int32_t virtio_gpu_driver::destroy_resource(const virtio_gpu_buffer& buf, wait_mode mode) {
    virtio_gpu_resource_detach_backing detach_backing = {};
    detach_backing.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    detach_backing.resource_id = buf.resource_id;

    virtio_gpu_resource_unref unref = {};
    unref.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.resource_id = buf.resource_id;

    if (queue_command(detach_backing) != OK ||
        queue_command(unref) != OK ||
        run_batch(mode) != OK) {
        return ERR_IO;
    }

    return OK;
}

// Queues the command that makes `scanout` show the whole of `buf`'s resource
int32_t virtio_gpu_driver::queue_set_scanout(uint32_t scanout, const virtio_gpu_buffer& buf) {
    virtio_gpu_set_scanout set_scanout = {};
    set_scanout.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    set_scanout.r           = {0, 0, buf.width, buf.height};
    set_scanout.scanout_id  = scanout;
    set_scanout.resource_id = buf.resource_id;

    return queue_command(set_scanout);
}

// Runs under `m_batch_lock` once the driver is attached, and takes `m_scanout_lock` too, so the display calls that must
// not wait read the base and the relighting mark under that lock alone
void virtio_gpu_driver::set_scanout_base(virtio_gpu_scanout& scanout, virtio_gpu_buffer* base, bool relighting) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_scanout_lock);
        scanout.base = base;
        scanout.is_relighting = relighting;
        sync::spin_unlock_irqrestore(m_scanout_lock, irq);
    });
}

// Allocates a base buffer for a scanout at `width` by `height`, or logs an error and returns null. Runs with no lock
// held, since the allocation zeroes the RAM, which under `m_batch_lock` would hold up every scanout's presents
virtio_gpu_buffer* virtio_gpu_driver::allocate_base(uint32_t index, uint32_t width, uint32_t height) {
    auto* base = heap::ualloc_new<virtio_gpu_buffer>();
    if (!base) {
        log::error("virtio-gpu: no memory for scanout %u's buffer record", index);
        return nullptr;
    }

    if (allocate_backing(*base, width, height, 0) != OK) {
        log::error("virtio-gpu: failed to allocate the RAM for scanout %u at %ux%u", index, width, height);
        heap::ufree_delete(base);
        return nullptr;
    }

    return base;
}

// Runs under `m_batch_lock` once the driver is attached. Lights a dark scanout with a buffer from `allocate_base()`.
// After a failure the device holds none of the buffer's RAM unless the result is ERR_RAM_HELD
int32_t virtio_gpu_driver::light_scanout(uint32_t index, virtio_gpu_buffer& base, wait_mode mode) {
    int32_t rc = create_resource(base, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, mode);
    if (rc == ERR_ATTACH_UNANSWERED) {
        return ERR_RAM_HELD;
    }

    if (rc != OK) {
        return ERR_IO;
    }

    if (queue_set_scanout(index, base) != OK || run_batch(mode) != OK) {
        return destroy_resource(base, mode) == OK ? ERR_IO : ERR_RAM_HELD;
    }

    virtio_gpu_scanout& scanout = m_scanouts[index];
    scanout.lit_ns = clock::now_ns();
    scanout.shown_resource_id = base.resource_id;
    set_scanout_base(scanout, &base, false);

    log::info("virtio-gpu: lit scanout %u at %ux%u", index, base.width, base.height);

    return OK;
}

// Runs at attach, which takes no lock. Lights a scanout with a new base buffer, holding the buffer in `m_held_base`
// when the device may still read it after a failed lighting, for `detach()` to free once it has reset the device
int32_t virtio_gpu_driver::light_at_attach(uint32_t index, uint32_t width, uint32_t height) {
    virtio_gpu_buffer* base = allocate_base(index, width, height);
    if (!base) {
        return ERR_IO;
    }

    int32_t rc = light_scanout(index, *base, wait_mode::poll);
    if (rc == ERR_RAM_HELD) {
        m_held_base = base;
    } else if (rc != OK) {
        free_buffer(base);
    }

    return rc;
}

// Runs at attach. Lights every scanout the device enables at a usable size, or scanout 0 at the default size when none
// is usable, so the display registers with a lit output
int32_t virtio_gpu_driver::light_enabled_scanouts(const virtio_gpu_display_one* modes) {
    bool has_lit_scanout = false;

    for (uint32_t i = 0; i < m_scanout_count; i++) {
        if (!is_usable_mode(modes[i])) {
            continue;
        }

        if (light_at_attach(i, modes[i].r.width, modes[i].r.height) != OK) {
            return ERR_IO;
        }

        has_lit_scanout = true;
    }

    if (has_lit_scanout) {
        return OK;
    }

    log::warn("virtio-gpu: no usable scanout reported, using %ux%u", DEFAULT_WIDTH, DEFAULT_HEIGHT);
    return light_at_attach(0, DEFAULT_WIDTH, DEFAULT_HEIGHT);
}

// Runs under `m_batch_lock` in the driver's task. Hides a lit scanout's cursor and has the device show nothing on it,
// then ends its unended present, if any, with GFXFB_PRESENT_DARK, so gfxfb may drop its buffers on the dark report
int32_t virtio_gpu_driver::darken_scanout(uint32_t index, bool relighting) {
    virtio_gpu_scanout& scanout = m_scanouts[index];
    uint64_t deadline_ns = clock::now_ns() + BATCH_TIMEOUT_NS;

    gfxfb::cursor_image hide = {};
    if (scanout.is_cursor_shown && update_cursor(scanout, hide, deadline_ns) != OK) {
        return ERR_IO;
    }

    // A scanout naming no resource shows nothing
    virtio_gpu_set_scanout show_nothing = {};
    show_nothing.hdr.type   = VIRTIO_GPU_CMD_SET_SCANOUT;
    show_nothing.scanout_id = index;

    if (queue_command(show_nothing) != OK || run_batch(wait_mode::sleep) != OK) {
        return ERR_IO;
    }

    gfxfb::present_result dark = {
        .status       = gfxfb::GFXFB_PRESENT_DARK,
        .frame        = 0,
        .timestamp_ns = 0,
    };

    // The latch timer's callback also ends a present only while `has_pending_latch` is set, so whichever of the two
    // clears it first ends the present
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_scanout_lock);
        scanout.base = nullptr;
        scanout.is_relighting = relighting;
        if (scanout.has_pending_latch) {
            scanout.has_pending_latch = false;
            gfxfb::present_done(this, index, dark);
        }

        sync::spin_unlock_irqrestore(m_scanout_lock, irq);

        timer::cancel(&scanout.latch_timer);
    });

    scanout.shown_resource_id = 0;
    log::info("virtio-gpu: scanout %u went dark%s", index, relighting ? " to light again at another size" : "");

    return OK;
}

// Creates the resource every scanout's cursor is drawn from, a picture of B8G8R8A8 pixels, which as little-endian
// 32-bit values hold alpha, red, green and blue from the top byte down, as gfxfb's cursor pixels do
int32_t virtio_gpu_driver::create_cursor_resource() {
    if (allocate_backing(m_cursor_buffer, CURSOR_DIMENSION, CURSOR_DIMENSION, paging::PAGE_USER) != OK) {
        log::error("virtio-gpu: failed to allocate the RAM for the cursor");
        return ERR_IO;
    }

    return create_resource(m_cursor_buffer, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM, wait_mode::poll);
}

// Runs under `m_batch_lock` between batches, sending `command` from the start of the command page. A command the device
// has not handed back by `answer_deadline(deadline_ns)` marks the device dead, as an unanswered batch does
int32_t virtio_gpu_driver::run_cursor_command(const virtio_gpu_update_cursor& command, uint64_t deadline_ns) {
    if (m_is_broken) {
        return ERR_IO;
    }

    string::memcpy(reinterpret_cast<void*>(m_command_page.virt), &command, sizeof(command));
    if (m_cursor_queue.add_buf(m_command_page.phys, sizeof(command), 0) < 0) {
        return ERR_IO;
    }

    m_cursor_queue.kick(m_cursor_notify);

    if (wait_for_used(m_cursor_queue, 1, wait_mode::sleep, answer_deadline(deadline_ns)) != OK) {
        log::error("virtio-gpu: device stopped answering cursor commands");
        m_is_broken = true;
        return ERR_IO;
    }

    return OK;
}

// A cursor command of `type` for a scanout, with a picture's hotspot at the hotspot's recorded position. It names the
// cursor resource even for a move, since the device hides the cursor on a move naming none
virtio_gpu_update_cursor virtio_gpu_driver::cursor_command(
    const virtio_gpu_scanout& scanout,
    uint32_t type,
    uint32_t hot_x,
    uint32_t hot_y
) const {
    virtio_gpu_update_cursor command = {};
    command.hdr.type       = type;
    command.pos.scanout_id = scanout.index;
    command.pos.x          = cursor_corner(scanout.cursor_x, hot_x);
    command.pos.y          = cursor_corner(scanout.cursor_y, hot_y);
    command.resource_id    = m_cursor_buffer.resource_id;
    command.hot_x          = hot_x;
    command.hot_y          = hot_y;

    return command;
}

// Runs under `m_batch_lock`. Shows `image` as a scanout's cursor, or hides it at a width of 0, the cursor command due
// by `deadline_ns`. The device reads the resource as it runs the command, so the copy is confirmed first
int32_t virtio_gpu_driver::update_cursor(
    virtio_gpu_scanout& scanout,
    const gfxfb::cursor_image& image,
    uint64_t deadline_ns
) {
    if (image.width == 0) {
        virtio_gpu_update_cursor hide = cursor_command(scanout, VIRTIO_GPU_CMD_UPDATE_CURSOR, 0, 0);
        hide.resource_id = 0;

        int32_t rc = run_cursor_command(hide, deadline_ns);
        if (rc == OK) {
            scanout.is_cursor_shown = false;
        }

        return rc;
    }

    auto* pixels = reinterpret_cast<uint32_t*>(m_cursor_buffer.backing.virt);
    string::memset(pixels, 0, m_cursor_buffer.size);
    for (uint32_t row = 0; row < image.height; row++) {
        uint32_t* destination = pixels + row * CURSOR_DIMENSION;
        const uint32_t* source = image.pixels + row * image.width;
        string::memcpy(destination, source, image.width * BYTES_PER_PIXEL);
    }

    virtio_gpu_transfer_to_host_2d transfer = {};
    transfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r           = {0, 0, CURSOR_DIMENSION, CURSOR_DIMENSION};
    transfer.resource_id = m_cursor_buffer.resource_id;

    // A copy the device answers by its deadline leaves the cursor command its minimum wait before `deadline_ns`
    uint64_t copy_deadline_ns = deadline_ns - MIN_ANSWER_WAIT_NS;
    if (queue_command(transfer) != OK || run_batch_until(wait_mode::sleep, copy_deadline_ns) != OK) {
        return ERR_IO;
    }

    virtio_gpu_update_cursor show = cursor_command(scanout, VIRTIO_GPU_CMD_UPDATE_CURSOR, image.hot_x, image.hot_y);
    int32_t rc = run_cursor_command(show, deadline_ns);
    if (rc == OK) {
        scanout.cursor_hot_x = image.hot_x;
        scanout.cursor_hot_y = image.hot_y;
        scanout.is_cursor_shown = true;
    }

    return rc;
}

// Queues the copy of `rect` from `buf`'s RAM into its resource and the flush that shows it on any scanout showing that
// resource, first running the batch with `deadline_ns` when the pair does not fit
int32_t virtio_gpu_driver::queue_rect_update(
    const virtio_gpu_buffer& buf,
    const gfxfb::gfxfb_rect& rect,
    uint64_t deadline_ns
) {
    uint64_t row_offset = static_cast<uint64_t>(rect.y) * buf.pitch;

    virtio_gpu_transfer_to_host_2d transfer = {};
    transfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r           = {rect.x, rect.y, rect.width, rect.height};
    transfer.offset      = row_offset + static_cast<uint64_t>(rect.x) * BYTES_PER_PIXEL;
    transfer.resource_id = buf.resource_id;

    virtio_gpu_resource_flush show = {};
    show.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    show.r           = transfer.r;
    show.resource_id = buf.resource_id;

    // Checked together so a rect is never copied without its flush
    uint32_t bytes = command_footprint(sizeof(transfer), sizeof(virtio_gpu_ctrl_hdr)) +
                     command_footprint(sizeof(show), sizeof(virtio_gpu_ctrl_hdr));

    if (!batch_has_room(bytes, COMMANDS_PER_RECT) && run_batch_until(wait_mode::sleep, deadline_ns) != OK) {
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
        setup_queue(VIRTIO_GPU_QUEUE_CURSOR, m_cursor_queue, &m_cursor_notify) != OK ||
        allocate_command_page() != OK) {
        set_failed();
        return ERR_IO;
    }

    // Without MSI-X, or when the device refuses the vector for either queue, every wait polls. The queues share one
    // vector, so a wait must sleep on it only under `m_batch_lock`
    bool has_config_interrupt = false;
    if (setup_msix(MSIX_VECTORS) == OK) {
        m_has_completion_interrupt = route_queue_msix(VIRTIO_GPU_QUEUE_CONTROL, COMPLETION_VECTOR) == OK &&
                                     route_queue_msix(VIRTIO_GPU_QUEUE_CURSOR, COMPLETION_VECTOR) == OK;

        // A device whose table holds one vector has none for configuration changes, and routing to one past the table
        // logs a refusal
        bool has_config_vector = m_dev->get_msi_state().vector_count > CONFIG_VECTOR;
        has_config_interrupt = has_config_vector && route_config_msix(CONFIG_VECTOR) == OK;
    }

    if (!m_has_completion_interrupt) {
        log::warn("virtio-gpu: no completion interrupt, polling instead");
    }

    if (!has_config_interrupt) {
        log::warn("virtio-gpu: no configuration change interrupt, so the scanouts stay as attach lit them");
    }

    set_driver_ok();

    m_scanout_count = read_scanout_count();

    virtio_gpu_display_one modes[VIRTIO_GPU_MAX_SCANOUTS] = {};
    if (read_display_info(wait_mode::poll, modes) != OK ||
        light_enabled_scanouts(modes) != OK ||
        create_cursor_resource() != OK) {
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
    log::info("virtio-gpu: %u scanout(s), device is live", m_scanout_count);

    return OK;
}

int32_t virtio_gpu_driver::detach() {
    // gfxfb can still call flush(), which must fail once the page is gone
    m_is_broken = true;
    reset_device();

    // gfxfb holds a registered display's lit base buffers, and hands one back only after a report that its scanout went
    // dark, which a detached driver never makes
    if (!m_is_registered) {
        for (uint32_t i = 0; i < m_scanout_count; i++) {
            if (m_scanouts[i].base) {
                free_buffer(m_scanouts[i].base);
                m_scanouts[i].base = nullptr;
            }
        }
    }

    if (m_held_base) {
        free_buffer(m_held_base);
        m_held_base = nullptr;
    }

    RUN_ELEVATED(dma::free_pages(m_cursor_buffer.backing));
    RUN_ELEVATED(dma::free_pages(m_command_page));

    return pci_driver::detach();
}

// Sleeps until the interrupt handler noted a configuration change or gfxfb handed a buffer back, then takes whichever
// of the two it finds, the change and the oldest buffer
virtio_gpu_driver::task_work virtio_gpu_driver::wait_for_work() {
    task_work work = {};

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_work_lock);
        while (!m_has_config_change && m_releases.empty()) {
            irq = sync::wait(m_work_wq, m_work_lock, irq);
        }

        work.has_config_change = m_has_config_change;
        work.released = m_releases.pop_front();
        m_has_config_change = false;
        sync::spin_unlock_irqrestore(m_work_lock, irq);
    });

    return work;
}

// Runs in the driver's task. Brings one scanout to what the device reports for it, reporting each change to gfxfb with
// no lock held, since gfxfb reads the scanout back under `m_scanout_lock` and allocates before returning
void virtio_gpu_driver::update_scanout(uint32_t index, const virtio_gpu_display_one& reported) {
    virtio_gpu_scanout& scanout = m_scanouts[index];
    uint32_t width = reported.r.width;
    uint32_t height = reported.r.height;
    bool is_usable = is_usable_mode(reported);

    // Only the task writes a scanout's base once the driver is attached, so it reads it with no lock
    virtio_gpu_buffer* base = scanout.base;
    bool has_reported_size = base && base->width == width && base->height == height;
    bool is_unchanged = base ? is_usable && has_reported_size : !is_usable;
    if (is_unchanged) {
        return;
    }

    bool is_too_large = width > MAX_DIMENSION || height > MAX_DIMENSION;
    if (reported.enabled && is_too_large) {
        log::warn(
            "virtio-gpu: scanout %u reported at %ux%u, past the driver's limit of %ux%u",
            index,
            width,
            height,
            MAX_DIMENSION,
            MAX_DIMENSION
        );
    }

    // Allocated before the darkening, so zeroing the RAM does not lengthen the time the scanout shows nothing
    virtio_gpu_buffer* new_base = is_usable ? allocate_base(index, width, height) : nullptr;

    if (base) {
        RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
        int32_t rc = darken_scanout(index, new_base != nullptr);
        RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

        // The device may still show the base buffer, so the scanout stays lit
        if (rc != OK) {
            log::error("virtio-gpu: failed to take scanout %u dark", index);
            if (new_base) {
                free_buffer(new_base);
            }

            return;
        }

        RUN_ELEVATED(gfxfb::output_changed(this, index));
    }

    if (!new_base) {
        return;
    }

    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
    int32_t rc = light_scanout(index, *new_base, wait_mode::sleep);
    if (rc != OK) {
        set_scanout_base(scanout, nullptr, false);
    }

    RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

    if (rc != OK) {
        log::error("virtio-gpu: failed to light scanout %u at %ux%u", index, width, height);
    }

    if (rc == ERR_RAM_HELD) {
        log_kept_ram(*new_base);
    } else if (rc != OK) {
        free_buffer(new_base);
    }

    // A scanout reported dark while relighting is reported again, lit or else dark without `relighting`
    if (rc == OK || base) {
        RUN_ELEVATED(gfxfb::output_changed(this, index));
    }
}

// Runs in the driver's task. Clears the display event before reading the display info, so a change the read misses
// raises the event again, then brings each scanout to what the device reports
void virtio_gpu_driver::apply_config_change() {
    volatile virtio_gpu_config* cfg = device_config<virtio_gpu_config>();
    if (!cfg || (cfg->events_read & VIRTIO_GPU_EVENT_DISPLAY) == 0) {
        return;
    }

    cfg->events_clear = VIRTIO_GPU_EVENT_DISPLAY;

    virtio_gpu_display_one modes[VIRTIO_GPU_MAX_SCANOUTS] = {};

    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
    int32_t rc = read_display_info(wait_mode::sleep, modes);
    RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

    if (rc != OK) {
        log::error("virtio-gpu: failed to read the scanouts after a configuration change");
        return;
    }

    for (uint32_t i = 0; i < m_scanout_count; i++) {
        update_scanout(i, modes[i]);
    }
}

// Follows the device's configuration changes and releases the buffers gfxfb handed back, since both wait for the
// device, which neither the interrupt handler nor the reaper may
void virtio_gpu_driver::run() {
    while (true) {
        task_work work = wait_for_work();

        if (work.has_config_change) {
            apply_config_change();
        }

        if (!work.released) {
            continue;
        }

        RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
        int32_t rc = destroy_resource(*work.released, wait_mode::sleep);
        RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

        // Unless the device confirmed both commands, the resource may still hold the RAM
        if (rc != OK) {
            log_kept_ram(*work.released);
            continue;
        }

        free_buffer(work.released);
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

    if (allocate_backing(*buf, width, height, 0) != OK) {
        heap::ufree_delete(buf);
        return gfxfb::ERR_NO_MEMORY;
    }

    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
    int32_t rc = create_resource(*buf, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, wait_mode::sleep);
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

    free_buffer(buf);

    if (rc == ERR_OUT_OF_DEVICE_MEMORY) {
        return gfxfb::ERR_NO_MEMORY;
    }

    // A device still answering refused the command, as it refuses a resource ID still in use once the IDs wrap
    return is_broken ? gfxfb::ERR_DEVICE : gfxfb::ERR_REFUSED;
}

// Runs under `m_batch_lock`. The device runs every command of a batch, so the scanout switches to the buffer's resource
// in a batch after the copies, which leaves the scanout on the resource it showed when a copy is refused
int32_t virtio_gpu_driver::show_buffer(
    virtio_gpu_scanout& scanout,
    const virtio_gpu_buffer& buf,
    const gfxfb::gfxfb_rect* regions,
    uint32_t count
) {
    uint64_t deadline_ns = clock::now_ns() + PRESENT_BATCH_TIMEOUT_NS;

    gfxfb::gfxfb_rect whole = {0, 0, buf.width, buf.height};
    if (count == 0) {
        regions = &whole;
        count = 1;
    }

    int32_t rc = OK;
    for (uint32_t i = 0; i < count && rc == OK; i++) {
        rc = queue_rect_update(buf, regions[i], deadline_ns);
    }

    if (rc == OK) {
        rc = run_batch_until(wait_mode::sleep, deadline_ns);
    }

    if (rc != OK || buf.resource_id == scanout.shown_resource_id) {
        return rc;
    }

    if (queue_set_scanout(scanout.index, buf) != OK) {
        return ERR_IO;
    }

    rc = run_batch_until(wait_mode::sleep, deadline_ns);
    if (rc == OK) {
        scanout.shown_resource_id = buf.resource_id;
    }

    return rc;
}

// Runs under `m_batch_lock` once the device confirmed every command of a present. Records the scanout's next refresh
// boundary as the present's latch, for `report_latch()` to report when it comes
void virtio_gpu_driver::set_latch_timer(virtio_gpu_scanout& scanout) {
    uint64_t now_ns = clock::now_ns();
    uint64_t since_lit_ns = now_ns > scanout.lit_ns ? now_ns - scanout.lit_ns : 0;
    uint64_t latch_frame = since_lit_ns / REFRESH_PERIOD_NS + 1;
    uint64_t latch_ns = scanout.lit_ns + latch_frame * REFRESH_PERIOD_NS;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_scanout_lock);
        scanout.has_pending_latch = true;
        scanout.latch_frame = latch_frame;
        scanout.latch_ns = latch_ns;
        sync::spin_unlock_irqrestore(m_scanout_lock, irq);

        timer::schedule(&scanout.latch_timer, latch_ns);
    });
}

// Reports the present the device confirmed last on a scanout as latched at the refresh boundary `set_latch_timer()`
// recorded, unless the scanout went dark first or the callback runs late for a present that has already ended
void virtio_gpu_driver::report_latch(timer::deadline_timer* latch_timer) {
    auto* scanout = timer::owner_of<virtio_gpu_scanout, &virtio_gpu_scanout::latch_timer>(latch_timer);
    virtio_gpu_driver* driver = scanout->driver;

    gfxfb::present_result latched = {};
    latched.status = gfxfb::GFXFB_PRESENT_LATCHED;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(driver->m_scanout_lock);
        bool is_due = scanout->has_pending_latch && clock::now_ns() >= scanout->latch_ns;
        if (is_due) {
            latched.frame = scanout->latch_frame;
            latched.timestamp_ns = scanout->latch_ns;
            scanout->has_pending_latch = false;
            gfxfb::present_done(driver, scanout->index, latched);
        }

        sync::spin_unlock_irqrestore(driver->m_scanout_lock, irq);
    });
}

int32_t virtio_gpu_driver::present(
    uint32_t output,
    gfxfb::buffer* buf,
    const gfxfb::gfxfb_rect* regions,
    uint32_t region_count
) {
    const auto& presented = *static_cast<virtio_gpu_buffer*>(buf);
    virtio_gpu_scanout& scanout = m_scanouts[output];

    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));

    const virtio_gpu_buffer* base = scanout.base;
    bool has_scanout_size = base && presented.width == base->width && presented.height == base->height;
    int32_t rc = gfxfb::OK;
    if (m_is_broken) {
        rc = gfxfb::ERR_DEVICE;
    } else if (!base) {
        rc = gfxfb::ERR_DARK;
    } else if (!has_scanout_size) {
        rc = gfxfb::ERR_INVALID_ARG;
    } else if (show_buffer(scanout, presented, regions, region_count) != OK && !m_is_broken) {
        rc = gfxfb::ERR_REFUSED;
    }

    bool missed_deadline = rc == gfxfb::OK && m_is_broken;
    if (rc == gfxfb::OK && !missed_deadline) {
        set_latch_timer(scanout);
    }

    RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

    // The device was given the buffer and may still read it, so the present ends failed instead of being refused
    if (missed_deadline) {
        gfxfb::present_result failed = {
            .status       = gfxfb::GFXFB_PRESENT_FAILED,
            .frame        = 0,
            .timestamp_ns = 0,
        };

        RUN_ELEVATED(gfxfb::present_done(this, output, failed));
    }

    return rc;
}

int32_t virtio_gpu_driver::set_cursor(uint32_t output, const gfxfb::cursor_image& image) {
    bool fits = image.width <= CURSOR_DIMENSION && image.height <= CURSOR_DIMENSION;
    virtio_gpu_scanout& scanout = m_scanouts[output];

    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
    uint64_t deadline_ns = clock::now_ns() + gfxfb::GFXFB_PRESENT_TIMEOUT_NS;

    int32_t rc = gfxfb::OK;
    if (m_is_broken) {
        rc = gfxfb::ERR_DEVICE;
    } else if (!scanout.base) {
        rc = gfxfb::ERR_DARK;
    } else if (!fits) {
        rc = gfxfb::ERR_INVALID_ARG;
    } else if (update_cursor(scanout, image, deadline_ns) != OK) {
        rc = m_is_broken ? gfxfb::ERR_DEVICE : gfxfb::ERR_REFUSED;
    }

    RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

    return rc;
}

int32_t virtio_gpu_driver::move_cursor(uint32_t output, int32_t x, int32_t y) {
    virtio_gpu_scanout& scanout = m_scanouts[output];

    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));
    uint64_t deadline_ns = clock::now_ns() + gfxfb::GFXFB_PRESENT_TIMEOUT_NS;

    int32_t rc = gfxfb::OK;
    if (m_is_broken) {
        rc = gfxfb::ERR_DEVICE;
    } else if (!scanout.base) {
        rc = gfxfb::ERR_DARK;
    } else {
        scanout.cursor_x = x;
        scanout.cursor_y = y;

        virtio_gpu_update_cursor move = cursor_command(
            scanout,
            VIRTIO_GPU_CMD_MOVE_CURSOR,
            scanout.cursor_hot_x,
            scanout.cursor_hot_y
        );

        if (scanout.is_cursor_shown && run_cursor_command(move, deadline_ns) != OK) {
            rc = m_is_broken ? gfxfb::ERR_DEVICE : gfxfb::ERR_REFUSED;
        }
    }

    RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));

    return rc;
}

void virtio_gpu_driver::release_buffer(gfxfb::buffer* buf) {
    auto* released = static_cast<virtio_gpu_buffer*>(buf);

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_work_lock);
        m_releases.push_back(released);
        sync::wake_one(m_work_wq);
        sync::spin_unlock_irqrestore(m_work_lock, irq);
    });
}

int32_t virtio_gpu_driver::read_output(uint32_t output, gfxfb::output_state& out) {
    if (output >= m_scanout_count) {
        return gfxfb::ERR_INVALID_ARG;
    }

    const virtio_gpu_scanout& scanout = m_scanouts[output];
    bool is_lit = false;
    bool is_relighting = false;
    uint32_t width = 0;
    uint32_t height = 0;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_scanout_lock);
        is_lit = scanout.base != nullptr;
        is_relighting = scanout.is_relighting;
        if (is_lit) {
            width = scanout.base->width;
            height = scanout.base->height;
        }

        sync::spin_unlock_irqrestore(m_scanout_lock, irq);
    });

    // The device does not say which scanout the boot firmware lit
    out.lit = is_lit;
    out.relighting = is_relighting;
    out.boot_output = false;

    // Every scanout has a cursor, drawn from the cursor resource
    out.cursor_max_width = CURSOR_DIMENSION;
    out.cursor_max_height = CURSOR_DIMENSION;

    if (out.lit) {
        out.current = {
            .width              = width,
            .height             = height,
            .refresh_millihertz = REFRESH_MILLIHERTZ,
        };
    }

    return gfxfb::OK;
}

gfxfb::buffer* virtio_gpu_driver::base_buffer(uint32_t output) {
    gfxfb::buffer* base = nullptr;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_scanout_lock);
        base = m_scanouts[output].base;
        sync::spin_unlock_irqrestore(m_scanout_lock, irq);
    });

    return base;
}

int32_t virtio_gpu_driver::flush(uint32_t output, const gfxfb::gfxfb_rect* rects, uint32_t count) {
    RUN_ELEVATED(sync::mutex_lock(m_batch_lock));

    const virtio_gpu_buffer* base = m_scanouts[output].base;
    if (!base) {
        RUN_ELEVATED(sync::mutex_unlock(m_batch_lock));
        return gfxfb::ERR_DARK;
    }

    // gfxfb names regions of the base buffer it read, which a relight may have replaced since
    int32_t rc = OK;
    for (uint32_t i = 0; i < count && rc == OK; i++) {
        gfxfb::gfxfb_rect clipped = clip_to_buffer(rects[i], *base);
        if (clipped.width != 0) {
            rc = queue_rect_update(*base, clipped, clock::now_ns() + BATCH_TIMEOUT_NS);
        }
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
    ack_interrupt();

    if (vector != CONFIG_VECTOR) {
        return;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(m_work_lock);
    m_has_config_change = true;
    sync::wake_one(m_work_wq);
    sync::spin_unlock_irqrestore(m_work_lock, irq);
}

REGISTER_PCI_DRIVER(virtio_gpu_driver,
    PCI_MATCH(VIRTIO_VENDOR_ID, VIRTIO_DEV_GPU, PCI_MATCH_ANY_8, PCI_MATCH_ANY_8, PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(virtio_gpu_driver));

} // namespace drivers
