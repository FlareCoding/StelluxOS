#include "drivers/graphics/gfxfb.h"
#include "boot/boot_services.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/gfxfb_node.h"
#include "fs/devfs/devfs.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "fs/node.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "mm/uaccess.h"
#include "mm/vma.h"
#include "sched/sched.h"
#include "signals/signal.h"
#include "sync/poll.h"

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

// True when each of the `count` regions covers at least one pixel and lies wholly inside `buf`, compared without adding
// a coordinate to a size, since the sum could wrap
static bool are_regions_nonempty_inside_buffer(const gfxfb_rect* regions, uint32_t count, const buffer& buf) {
    for (uint32_t i = 0; i < count; i++) {
        const gfxfb_rect& region = regions[i];
        bool covers_pixels = region.width > 0 && region.height > 0;
        bool fits_across = region.x < buf.width && region.width <= buf.width - region.x;
        bool fits_down = region.y < buf.height && region.height <= buf.height - region.y;
        if (!covers_pixels || !fits_across || !fits_down) {
            return false;
        }
    }

    return true;
}

// Copies the regions `args` names onto the unprivileged heap, as userland supplies them and they can fill 4 KiB, and
// checks each against `buf`. On OK the caller frees `regions`, left as passed when `args` names no region
static int32_t copy_present_regions(const gfxfb_present_args& args, const buffer& buf, gfxfb_rect*& regions) {
    if (args.region_count == 0) {
        return fs::OK;
    }

    size_t bytes = args.region_count * sizeof(gfxfb_rect);
    auto* copy = static_cast<gfxfb_rect*>(heap::ualloc(bytes));
    if (!copy) {
        return fs::ERR_NOMEM;
    }

    int32_t rc = mm::uaccess::copy_from_user(copy, reinterpret_cast<const void*>(args.regions), bytes);
    if (rc != mm::uaccess::OK || !are_regions_nonempty_inside_buffer(copy, args.region_count, buf)) {
        heap::ufree(copy);
        return fs::ERR_INVAL;
    }

    regions = copy;

    return fs::OK;
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
    case ERR_REFUSED:     return fs::ERR_IO;
    default:              return fs::ERR_IO;
    }
}

// A cursor maximum as `read_output` reports it, held to CURSOR_DIMENSION_MAX so the pictures gfxfb copies stay bounded
// and GFXFB_GET_OUTPUT reports the largest picture GFXFB_SET_CURSOR takes
static uint32_t bounded_cursor_max(uint32_t reported) {
    return reported < CURSOR_DIMENSION_MAX ? reported : CURSOR_DIMENSION_MAX;
}

// Copies the picture `args` shows onto the unprivileged heap, as userland supplies it and it can fill 256 KiB, once its
// hotspot lies inside it and it fits the output's cursor maximum. On OK the caller frees `pixels`
static int32_t copy_cursor_picture(display& shown_display, const gfxfb_cursor_args& args, uint32_t*& pixels) {
    bool hotspot_inside = args.hot_x < args.width && args.hot_y < args.height;
    if (!hotspot_inside) {
        return fs::ERR_INVAL;
    }

    output_state state = {};
    int32_t rc = shown_display.read_output(args.output, state);
    if (rc != OK) {
        return to_fs_error(rc);
    }

    bool fits_across = args.width <= bounded_cursor_max(state.cursor_max_width);
    bool fits_down = args.height <= bounded_cursor_max(state.cursor_max_height);
    if (!fits_across || !fits_down) {
        return fs::ERR_INVAL;
    }

    size_t bytes = static_cast<size_t>(args.width) * args.height * sizeof(uint32_t);
    auto* copy = static_cast<uint32_t*>(heap::ualloc(bytes));
    if (!copy) {
        return fs::ERR_NOMEM;
    }

    rc = mm::uaccess::copy_from_user(copy, reinterpret_cast<const void*>(args.pixels), bytes);
    if (rc != mm::uaccess::OK) {
        heap::ufree(copy);
        return fs::ERR_INVAL;
    }

    pixels = copy;

    return fs::OK;
}

// The event reporting how the present of `handle` on `output` ended, with a frame and a time only for one that latched
static gfxfb_event present_event(uint32_t output, uint32_t handle, const present_result& result) {
    bool latched = result.status == GFXFB_PRESENT_LATCHED;

    return {
        .type         = GFXFB_EVENT_PRESENT,
        .output       = output,
        .status       = result.status,
        .handle       = handle,
        .frame        = latched ? result.frame : 0,
        .timestamp_ns = latched ? result.timestamp_ns : 0,
    };
}

// The event naming `output` as changed, which carries nothing else
static gfxfb_event output_event(uint32_t output) {
    return {
        .type         = GFXFB_EVENT_OUTPUT,
        .output       = output,
        .status       = 0,
        .handle       = 0,
        .frame        = 0,
        .timestamp_ns = 0,
    };
}

// True while `owner` holds an unread event of its present on `output`
static bool has_unread_present_event(client& owner, uint32_t output) {
    bool found = false;

    sync::irq_state state = sync::spin_lock_irqsave(owner.events_lock);
    for (uint32_t i = 0; i < owner.event_count && !found; i++) {
        const gfxfb_event& event = owner.events[(owner.first_event + i) % CLIENT_EVENTS_MAX];
        found = event.type == GFXFB_EVENT_PRESENT && event.output == output;
    }

    sync::spin_unlock_irqrestore(owner.events_lock, state);

    return found;
}

// Appends a present's end, `event`, to `owner`'s events, which always have room: a client holds at most one present
// event and one output event per output, since a present fails while its client holds the last one's event there
static void queue_event(client& owner, const gfxfb_event& event) {
    sync::irq_state state = sync::spin_lock_irqsave(owner.events_lock);
    owner.events[(owner.first_event + owner.event_count) % CLIENT_EVENTS_MAX] = event;
    owner.event_count++;
    sync::spin_unlock_irqrestore(owner.events_lock, state);
}

// Appends an output event for `output` to `owner`'s events, dropping the one `owner` holds for `output`, so a client
// holds at most one output event per output and reads it after the ends of that output's earlier presents
static void queue_output_event(client& owner, uint32_t output) {
    sync::irq_state state = sync::spin_lock_irqsave(owner.events_lock);

    uint32_t kept = 0;
    for (uint32_t i = 0; i < owner.event_count; i++) {
        const gfxfb_event& event = owner.events[(owner.first_event + i) % CLIENT_EVENTS_MAX];
        bool is_replaced = event.type == GFXFB_EVENT_OUTPUT && event.output == output;
        if (!is_replaced) {
            owner.events[(owner.first_event + kept) % CLIENT_EVENTS_MAX] = event;
            kept++;
        }
    }

    owner.events[(owner.first_event + kept) % CLIENT_EVENTS_MAX] = output_event(output);
    owner.event_count = kept + 1;

    sync::spin_unlock_irqrestore(owner.events_lock, state);
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
    out.relighting = false;
    out.boot_output = true;
    out.cursor_max_width = 0;
    out.cursor_max_height = 0;

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
    init_locks_and_client_list();
    show_from_start(&m_boot_display);
}

gfxfb_node::gfxfb_node(const char* name, display* driver_display)
    : fs::node(fs::node_type::char_device, nullptr, name) {
    init_locks_and_client_list();
    show_from_start(driver_display);
}

void gfxfb_node::init_locks_and_client_list() {
    m_clients_lock.init();
    for (sync::mutex& lock : m_present_locks) {
        lock.init();
    }

    for (sync::mutex& lock : m_cursor_locks) {
        lock.init();
    }

    m_outputs_lock = sync::SPINLOCK_INIT;
    m_clients.init();
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

// Shows `shown_display` with each lit output showing its base buffer, and `primary` as the primary output when it has
// one, or with neither for NO_OUTPUT. Returns ERR_NO_MEMORY, showing nothing new, when a base buffer cannot be recorded
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

    // Unlocked, since gfxfb shows a display only before any program opens the node and before the display reports an
    // output, so no present or report can be in flight
    for (uint32_t i = 0; i < GFXFB_OUTPUTS_MAX; i++) {
        rc::strong_ref<buffer_record> base;
        if (fresh[i]) {
            fresh[i]->take(bases[i]);
            base = rc::strong_ref<buffer_record>::adopt(fresh[i]);
        }

        m_outputs[i].shown = base;
        m_outputs[i].base = base;
    }

    bool has_primary = primary != NO_OUTPUT && m_outputs[primary].base;
    m_primary = has_primary ? primary : NO_OUTPUT;
    m_size = has_primary ? buffer_bytes(m_outputs[primary].base->layout()) : 0;
    m_display = shown_display;

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

    owner->events_lock = sync::SPINLOCK_INIT;
    owner->event_waiters.init();

    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    m_clients.push_back(owner);
    sync::spin_unlock_irqrestore(m_outputs_lock, state);

    f->set_private_data(owner);
    m_was_opened.store_relaxed(true);

    return fs::OK;
}

// Runs once no call holds the file any more, so its handles drop without the lock and none of its calls is admitted.
// Control held by the closing client ends before the free, or a client allocated at the freed address would inherit it
int32_t gfxfb_node::on_close(fs::file* f) {
    client* closing = &client_of(f);

    // Each accepted present's end queues an event in its client, which must outlive those ends and keeps control
    wait_for_unended_presents(*closing);

    sync::mutex_lock(m_clients_lock);
    bool is_controller = m_controller == closing;
    sync::mutex_unlock(m_clients_lock);

    // Control ends only after waiting for the base buffers' presents, so no other client's present comes before theirs
    if (is_controller) {
        give_outputs_back(*closing);

        sync::mutex_lock(m_clients_lock);
        m_controller = nullptr;
        m_control_taken = false;
        sync::mutex_unlock(m_clients_lock);
    }

    // Off the list before the free, so no output change queues an event in freed memory
    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    m_clients.remove(closing);
    sync::spin_unlock_irqrestore(m_outputs_lock, state);

    heap::kfree_delete(closing);

    return fs::OK;
}

// Waits, holding no lock while asleep, until the display has ended each present of `owner` it accepted
void gfxfb_node::wait_for_unended_presents(client& owner) {
    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    while (holds_unended_present(owner)) {
        state = sync::wait(owner.event_waiters, m_outputs_lock, state);
    }

    sync::spin_unlock_irqrestore(m_outputs_lock, state);
}

// Drops the events of `owner`, the controlling client, presents each output's recorded base buffer where the output
// shows another buffer, naming no region, and hides every cursor. Waits for the display to end the presents it accepts
void gfxfb_node::give_outputs_back(client& owner) {
    // No file reads `owner`'s events any more, and `queue_present()` refuses an output whose event `owner` holds unread
    sync::irq_state state = sync::spin_lock_irqsave(owner.events_lock);
    owner.event_count = 0;
    sync::spin_unlock_irqrestore(owner.events_lock, state);

    for (uint32_t output = 0; output < clamped_output_count(*m_display); output++) {
        rc::strong_ref<buffer_record> base;

        state = sync::spin_lock_irqsave(m_outputs_lock);
        if (m_outputs[output].shown != m_outputs[output].base) {
            base = m_outputs[output].base;
        }

        sync::spin_unlock_irqrestore(m_outputs_lock, state);

        if (base) {
            queue_present(owner, output, 0, base, nullptr, 0);
        }

        // The hide's result is dropped, since a close reports nothing. When the display refuses it with ERR_REFUSED,
        // the cursor stays until a later controller's GFXFB_SET_CURSOR on the output replaces it
        give_cursor_image(output, {});
    }

    wait_for_unended_presents(owner);
}

ssize_t gfxfb_node::read(fs::file* f, void* buf, size_t count, uint32_t flags) {
    if (count < sizeof(gfxfb_event)) {
        return fs::ERR_INVAL;
    }

    client& reader = client_of(f);
    bool may_wait = !(flags & fs::O_NONBLOCK);
    size_t taken = 0;

    sync::irq_state state = sync::spin_lock_irqsave(reader.events_lock);
    while (reader.event_count == 0 && may_wait && !signals::interrupt_pending(sched::current())) {
        state = sync::wait(reader.event_waiters, reader.events_lock, state);
    }

    for (; taken < count / sizeof(gfxfb_event) && reader.event_count > 0; taken++) {
        string::memcpy(
            static_cast<uint8_t*>(buf) + taken * sizeof(gfxfb_event),
            &reader.events[reader.first_event],
            sizeof(gfxfb_event));

        reader.first_event = (reader.first_event + 1) % CLIENT_EVENTS_MAX;
        reader.event_count--;
    }

    sync::spin_unlock_irqrestore(reader.events_lock, state);

    if (taken == 0) {
        return may_wait ? fs::ERR_INTR : fs::ERR_AGAIN;
    }

    return static_cast<ssize_t>(taken * sizeof(gfxfb_event));
}

uint32_t gfxfb_node::poll(fs::file* f, sync::poll_table* pt) {
    client& poller = client_of(f);
    if (pt) {
        sync::poll_subscribe(*pt, poller.event_waiters);
    }

    sync::irq_state state = sync::spin_lock_irqsave(poller.events_lock);
    bool holds_events = poller.event_count > 0;
    sync::spin_unlock_irqrestore(poller.events_lock, state);

    return holds_events ? sync::POLL_IN : 0;
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

    if (cmd == GFXFB_SET_CURSOR) {
        return set_cursor(client_of(f), arg);
    }

    if (cmd == GFXFB_MOVE_CURSOR) {
        return move_cursor(client_of(f), arg);
    }

    return fs::ERR_NOSYS;
}

// The primary output's index, NO_OUTPUT while the node has none
uint32_t gfxfb_node::primary_output() {
    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    uint32_t primary = m_primary;
    sync::spin_unlock_irqrestore(m_outputs_lock, state);

    return primary;
}

// The primary output's recorded base buffer, null while the node has no primary output or the primary output relights
rc::strong_ref<buffer_record> gfxfb_node::primary_base() {
    rc::strong_ref<buffer_record> found;

    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    if (m_primary != NO_OUTPUT) {
        found = m_outputs[m_primary].base;
    }

    sync::spin_unlock_irqrestore(m_outputs_lock, state);

    return found;
}

int32_t gfxfb_node::get_info(uint64_t info_addr) {
    if (info_addr == 0) {
        return fs::ERR_INVAL;
    }

    rc::strong_ref<buffer_record> primary = primary_base();
    if (!primary) {
        return fs::ERR_AGAIN;
    }

    const buffer& base = primary->layout();

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

    bool is_primary = info.output == primary_output();

    info.output_count = m_display->output_count();
    info.flags = (state.lit ? GFXFB_OUTPUT_LIT : 0) | (is_primary ? GFXFB_OUTPUT_PRIMARY : 0);
    info.reserved = 0;
    info.current = state.lit ? state.current : gfxfb_mode{};
    info.cursor_max_width = bounded_cursor_max(state.cursor_max_width);
    info.cursor_max_height = bounded_cursor_max(state.cursor_max_height);

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

    if (has_display_failed()) {
        return fs::ERR_IO;
    }

    // While the primary shows a created buffer and holds no present of the base buffer, a flush reaches no screen, so
    // it leaves the display free for presents, and the base buffer's next present passes the display no region
    uint32_t primary = NO_OUTPUT;
    rc::strong_ref<buffer_record> primary_record;
    bool reaches_screen = false;

    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    if (m_primary != NO_OUTPUT && m_outputs[m_primary].base) {
        output_records& records = m_outputs[m_primary];
        primary = m_primary;
        primary_record = records.base;
        reaches_screen = records.shown == records.base || records.queued == records.base;
        if (!reaches_screen) {
            records.base_stale = true;
        }
    }

    sync::spin_unlock_irqrestore(m_outputs_lock, state);

    // No primary output, or a relighting one, fails the flush with nothing marked stale
    if (!primary_record) {
        return fs::ERR_AGAIN;
    }

    if (!reaches_screen) {
        return fs::OK;
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

            if (clip_rect_to_buffer(rect, primary_record->layout())) {
                batch[kept++] = rect;
            }
        }

        if (kept > 0) {
            rc = m_display->flush(primary, batch, kept);
            if (rc != OK) {
                mark_failed_on_device_error(rc);
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
        mark_failed_on_device_error(rc);
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

    if (args.reserved != 0 || args.region_count > GFXFB_MAX_FLUSH_RECTS) {
        return fs::ERR_INVAL;
    }

    if (args.output >= clamped_output_count(*m_display)) {
        return fs::ERR_INVAL;
    }

    rc::strong_ref<buffer_record> record = find_buffer_to_present(owner, args);
    if (!record) {
        return args.handle == 0 ? fs::ERR_AGAIN : fs::ERR_INVAL;
    }

    gfxfb_rect* regions = nullptr;
    rc = copy_present_regions(args, record->layout(), regions);
    if (rc != fs::OK) {
        return rc;
    }

    rc = fs::ERR_BUSY;
    if (begin_controlled_call(owner)) {
        rc = queue_present(owner, args.output, args.handle, record, regions, args.region_count);
        end_controlled_call(rc == fs::OK);
    }

    if (regions) {
        heap::ufree(regions);
    }

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

// Hands the display `record`'s buffer for `output` with its changed regions, or none for a stale base buffer, unless
// the display failed (`fs::ERR_IO`), the output holds a present or `owner` holds an unread event there (`fs::ERR_BUSY`)
int32_t gfxfb_node::queue_present(
    client& owner,
    uint32_t output,
    uint32_t handle,
    const rc::strong_ref<buffer_record>& record,
    const gfxfb_rect* regions,
    uint32_t region_count
) {
    sync::mutex_lock(m_present_locks[output]);

    // The output holds the present before the display gets it, since the display may end it inside the call
    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    output_records& records = m_outputs[output];
    bool display_failed = m_display_failed;
    bool is_free = !records.queued && !has_unread_present_event(owner, output);
    bool passes_whole_buffer = false;
    if (!display_failed && is_free) {
        records.queued = record;
        records.presenter = &owner;
        records.presented_handle = handle;

        // A skipped flush left changes in the base buffer that no region names
        if (handle == 0) {
            passes_whole_buffer = records.base_stale;
            records.base_stale = false;
        }
    }

    sync::spin_unlock_irqrestore(m_outputs_lock, state);

    if (display_failed || !is_free) {
        sync::mutex_unlock(m_present_locks[output]);
        return display_failed ? fs::ERR_IO : fs::ERR_BUSY;
    }

    const gfxfb_rect* given_regions = passes_whole_buffer ? nullptr : regions;
    uint32_t given_count = passes_whole_buffer ? 0 : region_count;
    int32_t rc = m_display->present(output, record->display_buffer(), given_regions, given_count);

    // A present the display did not accept will get no end, and a stale base buffer stays stale
    rc::strong_ref<buffer_record> refused;
    if (rc != OK) {
        state = sync::spin_lock_irqsave(m_outputs_lock);
        refused.swap(records.queued);
        records.presenter = nullptr;

        if (passes_whole_buffer) {
            records.base_stale = true;
        }

        if (rc == ERR_DEVICE) {
            m_display_failed = true;
        }

        sync::spin_unlock_irqrestore(m_outputs_lock, state);
    }

    sync::mutex_unlock(m_present_locks[output]);

    return to_fs_error(rc);
}

int32_t gfxfb_node::set_cursor(client& owner, uint64_t args_addr) {
    gfxfb_cursor_args args{};
    int32_t rc = mm::uaccess::copy_from_user(
        &args, reinterpret_cast<const void*>(args_addr), sizeof(args));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    bool is_reserved_clear = args.reserved == 0;
    bool names_output = args.output < clamped_output_count(*m_display);
    if (!is_reserved_clear || !names_output) {
        return fs::ERR_INVAL;
    }

    // A width of 0 hides the cursor, so the picture's other fields go unread
    uint32_t* pixels = nullptr;
    cursor_image image = {};
    if (args.width > 0) {
        rc = copy_cursor_picture(*m_display, args, pixels);
        if (rc != fs::OK) {
            return rc;
        }

        image = {
            .pixels = pixels,
            .width  = args.width,
            .height = args.height,
            .hot_x  = args.hot_x,
            .hot_y  = args.hot_y,
        };
    }

    rc = fs::ERR_BUSY;
    if (begin_controlled_call(owner)) {
        rc = give_cursor_image(args.output, image);
        end_controlled_call(rc == fs::OK);
    }

    if (pixels) {
        heap::ufree(pixels);
    }

    return rc;
}

// Hands the display `image` for `output`'s cursor, a width of 0 hiding the cursor, unless the display failed
int32_t gfxfb_node::give_cursor_image(uint32_t output, const cursor_image& image) {
    sync::mutex_lock(m_cursor_locks[output]);

    int32_t rc = fs::ERR_IO;
    if (!has_display_failed()) {
        int32_t display_rc = m_display->set_cursor(output, image);
        mark_failed_on_device_error(display_rc);
        rc = to_fs_error(display_rc);
    }

    sync::mutex_unlock(m_cursor_locks[output]);

    return rc;
}

int32_t gfxfb_node::move_cursor(client& owner, uint64_t position_addr) {
    gfxfb_cursor_position position{};
    int32_t rc = mm::uaccess::copy_from_user(
        &position, reinterpret_cast<const void*>(position_addr), sizeof(position));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    bool is_reserved_clear = position.reserved == 0;
    bool names_output = position.output < clamped_output_count(*m_display);
    if (!is_reserved_clear || !names_output) {
        return fs::ERR_INVAL;
    }

    if (!begin_controlled_call(owner)) {
        return fs::ERR_BUSY;
    }

    sync::mutex_lock(m_cursor_locks[position.output]);

    rc = fs::ERR_IO;
    if (!has_display_failed()) {
        int32_t display_rc = m_display->move_cursor(position.output, position.x, position.y);
        mark_failed_on_device_error(display_rc);
        rc = to_fs_error(display_rc);
    }

    sync::mutex_unlock(m_cursor_locks[position.output]);
    end_controlled_call(rc == fs::OK);

    return rc;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void gfxfb_node::present_done(uint32_t output, const present_result& result) {
    rc::strong_ref<buffer_record> dropped;

    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    client* presenter = output < GFXFB_OUTPUTS_MAX ? m_outputs[output].presenter : nullptr;
    if (!presenter) {
        sync::spin_unlock_irqrestore(m_outputs_lock, state);
        log::error("gfxfb: dropped the end of a present on output %u, which holds no unended present", output);
        return;
    }

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
        // Both buffers stay held, since the failed hardware may still read either, and no later present reaches it
        m_display_failed = true;
        break;
    }

    // The wake stays under the lock, since a closing presenter may be freed as soon as it takes the lock and finds no
    // unended present of its own
    records.presenter = nullptr;
    queue_event(*presenter, present_event(output, records.presented_handle, result));
    sync::wake_all(presenter->event_waiters);

    sync::spin_unlock_irqrestore(m_outputs_lock, state);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void gfxfb_node::output_changed(uint32_t output) {
    if (output >= clamped_output_count(*m_display)) {
        log::error("gfxfb: dropped a change of output %u, which the display does not have", output);
        return;
    }

    output_state state = {};
    bool is_read = m_display->read_output(output, state) == OK;
    bool is_lit = is_read && state.lit;
    bool is_relighting = is_read && !state.lit && state.relighting;
    buffer* lit_base = is_lit ? m_display->base_buffer(output) : nullptr;

    // Allocated before the spinlock, as allocating may sleep, and freed unused unless the output lit with a new base
    // buffer on a display that has not failed
    buffer_record* fresh = lit_base ? heap::kalloc_new<buffer_record>(m_display) : nullptr;

    rc::strong_ref<buffer_record> dropped_base;
    rc::strong_ref<buffer_record> dropped_shown;

    sync::irq_state irq = sync::spin_lock_irqsave(m_outputs_lock);
    if (m_display_failed) {
        sync::spin_unlock_irqrestore(m_outputs_lock, irq);
        heap::kfree_delete(fresh);
        return;
    }

    // A report repeating a lit output's base buffer keeps its record, so the buffer never has two records to release
    output_records& records = m_outputs[output];
    bool keeps_base = lit_base && records.base && records.base->display_buffer() == lit_base;
    bool lacks_record = false;
    if (!keeps_base) {
        dropped_base.swap(records.base);
        records.base_stale = false;

        if (fresh) {
            fresh->take(lit_base);
            records.base = rc::strong_ref<buffer_record>::adopt(fresh);
            fresh = nullptr;
        }

        // A lit output keeps showing a created buffer that latched before its report, since its hardware still reads it
        bool shows_created = is_lit && records.shown && records.shown != dropped_base;
        if (!shows_created) {
            dropped_shown.swap(records.shown);
            records.shown = records.base;
        }

        lacks_record = lit_base && !records.base;
    }

    uint32_t gaining = NO_OUTPUT;
    if (m_primary == output && !records.base && !is_relighting) {
        m_primary = lowest_output_with_base();
        gaining = m_primary;
    } else if (m_primary == NO_OUTPUT && records.base) {
        m_primary = output;
    }

    bool has_primary_base = m_primary != NO_OUTPUT && m_outputs[m_primary].base;
    m_size = has_primary_base ? buffer_bytes(m_outputs[m_primary].base->layout()) : 0;

    for (client& listener : m_clients) {
        queue_output_event(listener, output);
        if (gaining != NO_OUTPUT) {
            queue_output_event(listener, gaining);
        }

        sync::wake_all(listener.event_waiters);
    }

    sync::spin_unlock_irqrestore(m_outputs_lock, irq);

    if (lacks_record) {
        log::error("gfxfb: no memory to record the base buffer of output %u, so it counts as dark", output);
    }

    heap::kfree_delete(fresh);
}

// The output with the lowest index that has a base buffer recorded, or NO_OUTPUT. The caller holds `m_outputs_lock`
uint32_t gfxfb_node::lowest_output_with_base() const {
    for (uint32_t i = 0; i < GFXFB_OUTPUTS_MAX; i++) {
        if (m_outputs[i].base) {
            return i;
        }
    }

    return NO_OUTPUT;
}

// True once a call of the shown display returned ERR_DEVICE or one of its presents ended failed
bool gfxfb_node::has_display_failed() {
    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    bool display_failed = m_display_failed;
    sync::spin_unlock_irqrestore(m_outputs_lock, state);

    return display_failed;
}

// Counts the shown display as failed when `rc`, the result of one of its calls, is ERR_DEVICE
void gfxfb_node::mark_failed_on_device_error(int32_t rc) {
    if (rc != ERR_DEVICE) {
        return;
    }

    sync::irq_state state = sync::spin_lock_irqsave(m_outputs_lock);
    m_display_failed = true;
    sync::spin_unlock_irqrestore(m_outputs_lock, state);
}

// True while an output holds a present of `owner` that the display has not ended. The caller holds `m_outputs_lock`
bool gfxfb_node::holds_unended_present(const client& owner) const {
    for (const output_records& records : m_outputs) {
        if (records.presenter == &owner) {
            return true;
        }
    }

    return false;
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

    if (handle == 0) {
        return primary_base();
    }

    rc::strong_ref<buffer_record> found;

    sync::mutex_lock(m_clients_lock);
    if (handle <= GFXFB_CLIENT_BUFFERS_MAX) {
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

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void present_done(display* driver_display, uint32_t output, const present_result& result) {
    if (!g_node || !g_node->shows(driver_display)) {
        log::error("gfxfb: dropped the end of a present on output %u of a display /dev/gfxfb does not show", output);
        return;
    }

    g_node->present_done(output, result);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void output_changed(display* driver_display, uint32_t output) {
    if (!g_node || !g_node->shows(driver_display)) {
        log::error("gfxfb: dropped a change of output %u of a display /dev/gfxfb does not show", output);
        return;
    }

    g_node->output_changed(output);
}

} // namespace gfxfb
