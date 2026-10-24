#include "drivers/graphics/gfxfb.h"
#include "boot/boot_services.h"
#include "common/logging.h"
#include "drivers/graphics/gfxfb_node.h"
#include "fs/devfs/devfs.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "mm/uaccess.h"
#include "mm/vma.h"

namespace gfxfb {

// Rects copied in and handed to the display per call, small enough for the kernel stack
constexpr uint32_t RECTS_PER_DISPLAY_FLUSH = 16;

// /dev/gfxfb's name, given to the node and to devfs alike
constexpr const char* NODE_NAME = "gfxfb";

// The pixel layout of a driver display's buffers, which GFXFB_GET_INFO reports
constexpr uint16_t DRIVER_BPP         = 32;
constexpr uint8_t  DRIVER_RED_SHIFT   = 16;
constexpr uint8_t  DRIVER_GREEN_SHIFT = 8;
constexpr uint8_t  DRIVER_BLUE_SHIFT  = 0;

// What `find_primary` returns for a display with no output lit
constexpr uint32_t NO_OUTPUT = 0xFFFFFFFF;

// What `take_handle` returns when every handle of a client is in use
constexpr uint32_t NO_HANDLE = 0;

// mmap maps handle h's buffer from offset h times this, which no buffer's size reaches, above the primary output's base
// buffer at offset 0
constexpr uint64_t BUFFER_MAP_STRIDE = 1ULL << 40;

__PRIVILEGED_BSS static gfxfb_node* g_node;

static uint64_t buffer_bytes(const buffer& buf) {
    return static_cast<uint64_t>(buf.pitch) * buf.height;
}

static client& client_of(fs::file* f) {
    return *static_cast<client*>(f->private_data());
}

// Trims `rect` to `buf` and returns false when nothing of it remains
static bool clip_rect_to_buffer(gfxfb_rect& rect, const buffer& buf) {
    if (rect.width == 0 || rect.height == 0 || rect.x >= buf.width || rect.y >= buf.height) {
        return false;
    }

    uint32_t max_width = buf.width - rect.x;
    if (rect.width > max_width) {
        rect.width = max_width;
    }

    uint32_t max_height = buf.height - rect.y;
    if (rect.height > max_height) {
        rect.height = max_height;
    }

    return true;
}

// `shown_display`'s output count, at most GFXFB_OUTPUTS_MAX, so gfxfb never indexes past its arrays
static uint32_t clamped_output_count(display& shown_display) {
    uint32_t count = shown_display.output_count();
    return count < GFXFB_OUTPUTS_MAX ? count : GFXFB_OUTPUTS_MAX;
}

// The lit boot output, else the lit output with the lowest index, else NO_OUTPUT
static uint32_t find_primary(display& shown_display) {
    uint32_t lowest_lit = NO_OUTPUT;

    for (uint32_t i = 0; i < clamped_output_count(shown_display); i++) {
        output_state state = {};
        if (shown_display.read_output(i, state) != OK || !state.lit) {
            continue;
        }

        if (state.boot_output) {
            return i;
        }

        if (lowest_lit == NO_OUTPUT) {
            lowest_lit = i;
        }
    }

    return lowest_lit;
}

// The file layer's code for each result a display's calls return, and ERR_IO for any other
static int32_t to_fs_error(int32_t rc) {
    switch (rc) {
    case OK:              return fs::OK;
    case ERR_INVALID_ARG: return fs::ERR_INVAL;
    case ERR_DARK:        return fs::ERR_AGAIN;
    case ERR_DEVICE:      return fs::ERR_IO;
    case ERR_NO_MEMORY:   return fs::ERR_NOMEM;
    case ERR_UNSUPPORTED: return fs::ERR_NOSYS;
    default:              return fs::ERR_IO;
    }
}

void buffer_record::take(buffer* buf) {
    m_buffer = buf;
    m_layout = *buf;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void buffer_record::release_memory() {
    m_display->release_buffer(m_buffer);
    heap::kfree_delete(this);
}

int32_t boot_display::read_output(uint32_t output, output_state& out) {
    if (output != 0) {
        return ERR_INVALID_ARG;
    }

    out.lit = true;
    out.boot_output = true;

    // The boot framebuffer carries no refresh rate
    out.current = {
        .width              = m_framebuffer.pixels.width,
        .height             = m_framebuffer.pixels.height,
        .refresh_millihertz = 0,
    };

    return OK;
}

buffer* boot_display::base_buffer(uint32_t output) {
    return output == 0 ? &m_framebuffer.pixels : nullptr;
}

int32_t boot_display::flush(uint32_t, const gfxfb_rect*, uint32_t) {
    return OK;
}

gfxfb_node::gfxfb_node(const char* name, const boot_framebuffer& boot_fb)
    : fs::node(fs::node_type::char_device, nullptr, name), m_boot_display(boot_fb) {
    init_locks();
    show_from_start(&m_boot_display);
}

gfxfb_node::gfxfb_node(const char* name, display* driver_display)
    : fs::node(fs::node_type::char_device, nullptr, name) {
    init_locks();
    show_from_start(driver_display);
}

void gfxfb_node::init_locks() {
    m_clients_lock.init();
    for (sync::mutex& lock : m_present_locks) {
        lock.init();
    }

    m_outputs_lock = sync::SPINLOCK_INIT;
}

int32_t gfxfb_node::register_display(display* driver_display) {
    if (has_driver_display()) {
        log::warn("gfxfb: a driver display is already registered");
        return ERR;
    }

    if (m_was_opened.load_relaxed()) {
        log::warn("gfxfb: refused a driver display, since a program has opened the device");
        return ERR;
    }

    uint32_t primary = find_primary(*driver_display);
    if (primary == NO_OUTPUT) {
        log::warn("gfxfb: refused a driver display with no output lit, keeping the boot framebuffer");
        return ERR;
    }

    if (show(driver_display, primary) != OK) {
        log::error("gfxfb: refused a driver display, since no memory is left to record its base buffers");
        return ERR;
    }

    return OK;
}

// Shows `shown_display` with `primary` as its primary output and each lit output showing its recorded base buffer, or
// with neither for NO_OUTPUT. Returns ERR_NO_MEMORY, showing nothing new, when a base buffer cannot be recorded
int32_t gfxfb_node::show(display* shown_display, uint32_t primary) {
    uint32_t count = primary == NO_OUTPUT ? 0 : clamped_output_count(*shown_display);
    buffer* bases[GFXFB_OUTPUTS_MAX] = {};
    buffer_record* fresh[GFXFB_OUTPUTS_MAX] = {};

    // Every record is allocated before any takes its buffer, since dropping a record that took one hands the buffer
    // back to a display that still shows it
    for (uint32_t i = 0; i < count; i++) {
        bases[i] = shown_display->base_buffer(i);
        if (bases[i]) {
            fresh[i] = heap::kalloc_new<buffer_record>(shown_display);
        }

        if (bases[i] && !fresh[i]) {
            for (buffer_record* unused : fresh) {
                heap::kfree_delete(unused);
            }

            return ERR_NO_MEMORY;
        }
    }

    // Unlocked, since gfxfb shows a display only before any program opens the node, so no present can be in flight
    for (uint32_t i = 0; i < GFXFB_OUTPUTS_MAX; i++) {
        rc::strong_ref<buffer_record> base;
        if (fresh[i]) {
            fresh[i]->take(bases[i]);
            base = rc::strong_ref<buffer_record>::adopt(fresh[i]);
        }

        m_outputs[i].shown = base;
        m_outputs[i].base = base;
    }

    m_display = shown_display;
    m_primary = primary;
    m_primary_record = primary == NO_OUTPUT ? rc::strong_ref<buffer_record>() : m_outputs[primary].base;
    m_size = m_primary_record ? buffer_bytes(m_primary_record->layout()) : 0;

    return OK;
}

// Shows `shown_display` from construction, without a primary output when a lit output's base buffer cannot be recorded
void gfxfb_node::show_from_start(display* shown_display) {
    if (show(shown_display, find_primary(*shown_display)) != OK) {
        log::error("gfxfb: no memory to record the lit outputs' base buffers, so the device has no primary output");
        show(shown_display, NO_OUTPUT);
    }
}

int32_t gfxfb_node::open(fs::file* f, uint32_t) {
    auto* owner = heap::kalloc_new<client>();
    if (!owner) {
        return fs::ERR_NOMEM;
    }

    f->set_private_data(owner);
    m_was_opened.store_relaxed(true);

    return fs::OK;
}

// Runs once no call holds the file any more, so its handles drop without the lock and none of its calls is admitted.
// Control held by the closing client ends before the free, or a client allocated at the freed address would inherit it
int32_t gfxfb_node::on_close(fs::file* f) {
    client* closing = &client_of(f);

    sync::mutex_lock(m_clients_lock);
    if (m_controller == closing) {
        m_controller = nullptr;
        m_control_taken = false;
    }

    sync::mutex_unlock(m_clients_lock);

    heap::kfree_delete(closing);

    return fs::OK;
}

int32_t gfxfb_node::ioctl(fs::file* f, uint32_t cmd, uint64_t arg) {
    if (cmd == GFXFB_GET_INFO) {
        return get_info(arg);
    }

    if (cmd == GFXFB_GET_OUTPUT) {
        return get_output(arg);
    }

    if (cmd == GFXFB_FLUSH) {
        return flush(arg);
    }

    if (cmd == GFXFB_CREATE_BUFFER) {
        return create_buffer(client_of(f), arg);
    }

    if (cmd == GFXFB_DESTROY_BUFFER) {
        return destroy_buffer(client_of(f), arg);
    }

    if (cmd == GFXFB_PRESENT) {
        return present(client_of(f), arg);
    }

    return fs::ERR_NOSYS;
}

int32_t gfxfb_node::get_info(uint64_t info_addr) {
    if (info_addr == 0) {
        return fs::ERR_INVAL;
    }

    if (!m_primary_record) {
        return fs::ERR_AGAIN;
    }

    const buffer& base = m_primary_record->layout();

    gfxfb_info info{};
    info.width = base.width;
    info.height = base.height;
    info.pitch = base.pitch;
    info.flags = m_display->needs_flush() ? GFXFB_INFO_NEEDS_FLUSH : 0;
    info.size = buffer_bytes(base);

    if (has_driver_display()) {
        info.bpp = DRIVER_BPP;
        info.red_shift = DRIVER_RED_SHIFT;
        info.green_shift = DRIVER_GREEN_SHIFT;
        info.blue_shift = DRIVER_BLUE_SHIFT;
    } else {
        const boot_framebuffer& fb = m_boot_display.framebuffer();
        info.bpp = fb.bpp;
        info.red_shift = fb.red_shift;
        info.green_shift = fb.green_shift;
        info.blue_shift = fb.blue_shift;
    }

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(info_addr), &info, sizeof(info));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    return fs::OK;
}

int32_t gfxfb_node::get_output(uint64_t info_addr) {
    gfxfb_output_info info{};
    int32_t rc = mm::uaccess::copy_from_user(
        &info, reinterpret_cast<const void*>(info_addr), sizeof(info));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    output_state state = {};
    rc = m_display->read_output(info.output, state);
    if (rc != OK) {
        return to_fs_error(rc);
    }

    bool is_primary = m_primary_record && info.output == m_primary;

    info.output_count = m_display->output_count();
    info.flags = (state.lit ? GFXFB_OUTPUT_LIT : 0) | (is_primary ? GFXFB_OUTPUT_PRIMARY : 0);
    info.reserved = 0;
    info.current = state.lit ? state.current : gfxfb_mode{};

    rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(info_addr), &info, sizeof(info));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    return fs::OK;
}

int32_t gfxfb_node::flush(uint64_t args_addr) {
    if (args_addr == 0) {
        return fs::ERR_INVAL;
    }

    gfxfb_flush_args args{};
    int32_t rc = mm::uaccess::copy_from_user(
        &args, reinterpret_cast<const void*>(args_addr), sizeof(args));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    if (args.reserved != 0 || args.count > GFXFB_MAX_FLUSH_RECTS) {
        return fs::ERR_INVAL;
    }

    if (!m_primary_record) {
        return fs::ERR_AGAIN;
    }

    gfxfb_rect batch[RECTS_PER_DISPLAY_FLUSH];
    uint32_t handled = 0;

    while (handled < args.count) {
        uint32_t batch_count = args.count - handled;
        if (batch_count > RECTS_PER_DISPLAY_FLUSH) {
            batch_count = RECTS_PER_DISPLAY_FLUSH;
        }

        uint64_t batch_addr = args.rects + static_cast<uint64_t>(handled) * sizeof(gfxfb_rect);
        rc = mm::uaccess::copy_from_user(
            batch, reinterpret_cast<const void*>(batch_addr), batch_count * sizeof(gfxfb_rect));

        if (rc != mm::uaccess::OK) {
            return fs::ERR_INVAL;
        }

        uint32_t kept = 0;
        for (uint32_t i = 0; i < batch_count; i++) {
            gfxfb_rect rect = batch[i];

            if (clip_rect_to_buffer(rect, m_primary_record->layout())) {
                batch[kept++] = rect;
            }
        }

        if (kept > 0) {
            rc = m_display->flush(m_primary, batch, kept);
            if (rc != OK) {
                return to_fs_error(rc);
            }
        }

        handled += batch_count;
    }

    return fs::OK;
}

int32_t gfxfb_node::create_buffer(client& owner, uint64_t args_addr) {
    gfxfb_buffer_args args{};
    int32_t rc = mm::uaccess::copy_from_user(
        &args, reinterpret_cast<const void*>(args_addr), sizeof(args));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    if (args.width == 0 || args.height == 0 || args.reserved != 0) {
        return fs::ERR_INVAL;
    }

    if (!begin_controlled_call(owner)) {
        return fs::ERR_BUSY;
    }

    rc = create_buffer_under_handle(owner, args, args_addr);
    end_controlled_call(rc == fs::OK);

    return rc;
}

// Creates the buffer `args` asks for, under a free handle of `owner`, and writes the reply to `args_addr`
int32_t gfxfb_node::create_buffer_under_handle(client& owner, gfxfb_buffer_args& args, uint64_t args_addr) {
    uint32_t handle = take_handle(owner);
    if (handle == NO_HANDLE) {
        return fs::ERR_NOMEM;
    }

    // The record comes first, so a buffer the display creates always has a record to hand it back through
    auto* record = heap::kalloc_new<buffer_record>(m_display);
    if (!record) {
        end_creation(owner, handle, nullptr);
        return fs::ERR_NOMEM;
    }

    buffer* created = nullptr;
    int32_t rc = m_display->create_buffer(args.width, args.height, args.format, &created);
    if (rc != OK) {
        heap::kfree_delete(record);
        end_creation(owner, handle, nullptr);
        return to_fs_error(rc);
    }

    record->take(created);

    args.handle = handle;
    args.pitch = record->layout().pitch;
    args.size = record->layout().size;
    args.map_offset = handle * BUFFER_MAP_STRIDE;

    rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(args_addr), &args, sizeof(args));

    if (rc != mm::uaccess::OK) {
        end_creation(owner, handle, nullptr);
        if (record->release()) {
            mm::device_backing::ref_destroy(record);
        }

        return fs::ERR_INVAL;
    }

    end_creation(owner, handle, record);

    return fs::OK;
}

int32_t gfxfb_node::destroy_buffer(client& owner, uint64_t handle) {
    rc::strong_ref<buffer_record> dropped;

    sync::mutex_lock(m_clients_lock);
    if (handle >= 1 && handle <= GFXFB_CLIENT_BUFFERS_MAX) {
        dropped.swap(owner.buffers[handle - 1]);
    }

    sync::mutex_unlock(m_clients_lock);

    return dropped ? fs::OK : fs::ERR_INVAL;
}

int32_t gfxfb_node::present(client& owner, uint64_t args_addr) {
    gfxfb_present_args args{};
    int32_t rc = mm::uaccess::copy_from_user(
        &args, reinterpret_cast<const void*>(args_addr), sizeof(args));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    if (args.output >= clamped_output_count(*m_display)) {
        return fs::ERR_INVAL;
    }

    rc::strong_ref<buffer_record> record = find_buffer_to_present(owner, args);
    if (!record) {
        return args.handle == 0 ? fs::ERR_AGAIN : fs::ERR_INVAL;
    }

    if (!begin_controlled_call(owner)) {
        return fs::ERR_BUSY;
    }

    rc = queue_present(args.output, record);
    end_controlled_call(rc == fs::OK);

    return rc;
}

// The buffer `args` presents: the handle's buffer of `owner`, or the output's base buffer for handle 0. Null when the
// handle names no buffer of `owner`, or for handle 0 on an output with no base buffer recorded
rc::strong_ref<buffer_record> gfxfb_node::find_buffer_to_present(client& owner, const gfxfb_present_args& args) {
    rc::strong_ref<buffer_record> found;

    if (args.handle == 0) {
        sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
        found = m_outputs[args.output].base;
        sync::spin_unlock_irqrestore(m_outputs_lock, state);

        return found;
    }

    sync::mutex_lock(m_clients_lock);
    if (args.handle <= GFXFB_CLIENT_BUFFERS_MAX) {
        found = owner.buffers[args.handle - 1];
    }

    sync::mutex_unlock(m_clients_lock);

    return found;
}

// Gives `record`'s buffer to the display for `output`'s next vertical blank, or returns `fs::ERR_BUSY` while the output
// holds a present. The output holds the buffer before the call, since the display may end the present inside the call
int32_t gfxfb_node::queue_present(uint32_t output, const rc::strong_ref<buffer_record>& record) {
    sync::mutex_lock(m_present_locks[output]);

    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    bool is_free = !m_outputs[output].queued;
    if (is_free) {
        m_outputs[output].queued = record;
    }

    sync::spin_unlock_irqrestore(m_outputs_lock, state);

    if (!is_free) {
        sync::mutex_unlock(m_present_locks[output]);
        return fs::ERR_BUSY;
    }

    int32_t rc = m_display->present(output, record->display_buffer());

    // A refused present never reached the hardware, so no end will come for it
    rc::strong_ref<buffer_record> refused;
    if (rc != OK) {
        state = sync::spin_lock_irqsave(m_outputs_lock);
        refused.swap(m_outputs[output].queued);
        sync::spin_unlock_irqrestore(m_outputs_lock, state);
    }

    sync::mutex_unlock(m_present_locks[output]);

    return to_fs_error(rc);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void gfxfb_node::present_done(uint32_t output, const present_result& result) {
    rc::strong_ref<buffer_record> dropped;

    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    output_records& records = m_outputs[output];
    switch (result.status) {
    case GFXFB_PRESENT_LATCHED:
        dropped.swap(records.shown);
        records.shown.swap(records.queued);
        break;
    case GFXFB_PRESENT_DARK:
        dropped.swap(records.queued);
        break;
    case GFXFB_PRESENT_FAILED:
        // Both buffers stay held, since the failed hardware may still read either
        break;
    }

    sync::spin_unlock_irqrestore(m_outputs_lock, state);
}

// True when `caller`'s call is admitted, false while another client controls or reserves control. An admitted call
// reserves control for `caller`, takes control if it succeeds and must end through `end_controlled_call`
bool gfxfb_node::begin_controlled_call(client& caller) {
    sync::mutex_lock(m_clients_lock);
    if (!m_controller) {
        m_controller = &caller;
    }

    bool is_controller = m_controller == &caller;
    if (is_controller) {
        m_controller_calls++;
    }

    sync::mutex_unlock(m_clients_lock);

    return is_controller;
}

// Ends a call `begin_controlled_call` admitted: its client takes control when the call succeeded, and loses its
// reservation once the last of its admitted calls ends with none of them having succeeded
void gfxfb_node::end_controlled_call(bool succeeded) {
    sync::mutex_lock(m_clients_lock);
    m_controller_calls--;
    if (succeeded) {
        m_control_taken = true;
    }

    if (!m_control_taken && m_controller_calls == 0) {
        m_controller = nullptr;
    }

    sync::mutex_unlock(m_clients_lock);
}

// Holds the lowest free handle of `owner` for a creation, or returns NO_HANDLE when every handle is in use
uint32_t gfxfb_node::take_handle(client& owner) {
    uint32_t handle = NO_HANDLE;

    sync::mutex_lock(m_clients_lock);
    for (uint32_t i = 0; i < GFXFB_CLIENT_BUFFERS_MAX; i++) {
        if (!owner.buffers[i] && !owner.pending[i]) {
            owner.pending[i] = true;
            handle = i + 1;
            break;
        }
    }

    sync::mutex_unlock(m_clients_lock);

    return handle;
}

// Ends the creation holding `handle`, which then names `record` and takes over the caller's reference to it, or names
// nothing when `record` is null
void gfxfb_node::end_creation(client& owner, uint32_t handle, buffer_record* record) {
    sync::mutex_lock(m_clients_lock);
    owner.pending[handle - 1] = false;
    if (record) {
        owner.buffers[handle - 1] = rc::strong_ref<buffer_record>::adopt(record);
    }

    sync::mutex_unlock(m_clients_lock);
}

// The buffer mmap maps from `offset`, with `offset_in_buffer` set to where the offset falls in it, or null when the
// offset names no buffer of `owner` or primary output
rc::strong_ref<buffer_record> gfxfb_node::find_buffer_at(
    client& owner,
    uint64_t offset,
    uint64_t& offset_in_buffer
) {
    uint64_t handle = offset / BUFFER_MAP_STRIDE;
    offset_in_buffer = offset % BUFFER_MAP_STRIDE;

    rc::strong_ref<buffer_record> found;

    sync::mutex_lock(m_clients_lock);
    if (handle == 0) {
        found = m_primary_record;
    } else if (handle <= GFXFB_CLIENT_BUFFERS_MAX) {
        found = owner.buffers[handle - 1];
    }

    sync::mutex_unlock(m_clients_lock);

    return found;
}

int32_t gfxfb_node::mmap(fs::file* f, mm::mm_context* mm_ctx, uintptr_t addr,
                         size_t length, uint32_t prot, uint32_t map_flags,
                         uint64_t offset, uintptr_t* out_addr) {
    uint64_t offset_in_buffer = 0;
    rc::strong_ref<buffer_record> record = find_buffer_at(client_of(f), offset, offset_in_buffer);
    if (!record) {
        log::error("gfxfb: mmap: no buffer at offset 0x%lx", offset);
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_len = pmm::page_align_up(length);
    if (aligned_len < length) {
        log::error("gfxfb: mmap: aligned_len < length");
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    const buffer& layout = record->layout();
    if (aligned_len > layout.size || offset_in_buffer > layout.size - aligned_len) {
        log::error("gfxfb: mmap: %lu bytes from byte %lu end past the buffer's %lu bytes",
                   aligned_len, offset_in_buffer, layout.size);

        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    uint32_t cache = layout.cacheable ? paging::PAGE_NORMAL : paging::PAGE_WC;
    log::info("gfxfb: mmap: phys=0x%lx len=%lu prot=%u cache=%u",
              layout.phys + offset_in_buffer, length, prot, cache);

    int32_t rc = mm::mm_context_map_device(
        mm_ctx,
        layout.phys + offset_in_buffer,
        length,
        record.ptr(),
        offset_in_buffer,
        prot,
        cache,
        map_flags,
        addr,
        out_addr
    );

    if (rc != mm::MM_CTX_OK) {
        log::error("gfxfb: mmap: mm_context_map_device failed rc=%d", rc);
    }

    return rc;
}

/**
 * @brief Publish `node` as /dev/gfxfb and keep it as gfxfb's node, leaving it allocated when devfs refuses it.
 * @param node A node named `NODE_NAME` from `heap::kalloc_new`, or nullptr when the allocation failed.
 * @return OK, or ERR with an error logged when `node` is null or devfs refuses it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t publish_node(gfxfb_node* node) {
    if (!node) {
        log::error("gfxfb: failed to allocate gfxfb_node");
        return ERR;
    }

    // A refused node stays allocated, since freeing one that shows its boot display would drop the framebuffer's
    // record, and the reaper would then hand the framebuffer back to the boot display inside the freed node
    int32_t rc = devfs::add_char_device(NODE_NAME, node);
    if (rc != devfs::OK) {
        log::error("gfxfb: failed to register /dev/gfxfb");
        return ERR;
    }

    g_node = node;

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    auto& fb = g_boot_info.framebuffer;
    if (fb.fb_phys == 0) {
        log::info("gfxfb: no framebuffer available");
        return OK;
    }

    const boot_framebuffer boot_fb = {
        .pixels = {
            .width     = static_cast<uint32_t>(fb.width),
            .height    = static_cast<uint32_t>(fb.height),
            .pitch     = static_cast<uint32_t>(fb.pitch),
            .size      = pmm::page_align_up(fb.pitch * fb.height),
            .phys      = fb.fb_phys,
            .cacheable = false,
        },
        .bpp         = fb.bpp,
        .red_shift   = fb.red_mask_shift,
        .green_shift = fb.green_mask_shift,
        .blue_shift  = fb.blue_mask_shift,
    };

    if (publish_node(heap::kalloc_new<gfxfb_node>(NODE_NAME, boot_fb)) != OK) {
        return ERR;
    }

    log::info("gfxfb: %lux%lu %ubpp phys=0x%lx size=%lu, registered /dev/gfxfb",
              fb.width, fb.height, static_cast<unsigned int>(fb.bpp),
              fb.fb_phys, fb.pitch * fb.height);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool has_driver_display() {
    return g_node && g_node->has_driver_display();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool has_boot_display() {
    return g_node && !g_node->has_driver_display();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_display(display* driver_display) {
    if (!driver_display) {
        return ERR;
    }

    if (g_node) {
        if (g_node->register_display(driver_display) != OK) {
            return ERR;
        }

        log::info("gfxfb: a driver display replaced the boot framebuffer");
        return OK;
    }

    if (publish_node(heap::kalloc_new<gfxfb_node>(NODE_NAME, driver_display)) != OK) {
        return ERR;
    }

    log::info("gfxfb: registered /dev/gfxfb with a driver display");

    return OK;
}

} // namespace gfxfb
