#include "compare.hpp"
#include "decor.hpp"
#include "server.hpp"

#include <stlx/gpu_abi.h>
#include <stlxgfx/surface.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Toplevels cascade from here, expressed as the content origin: the
 * frame's outer corner is at (60, 48) from its output's top-left */
constexpr int32_t CASCADE_X = 60 + decor::BORDER;
constexpr int32_t CASCADE_Y = 48 + decor::TITLE_H;
constexpr int32_t CASCADE_STEP = 32;
constexpr uint32_t CASCADE_WRAP = 10;

/* Occlusion pieces per compose rect are bounded, a pathological
 * window arrangement falls back to overdraw instead of growing */
constexpr size_t OCCLUDE_PIECE_CAP = 64;

/* A pending GPU commit's acquire point is queried at every paced tick
 * for its first second, then every 100 ms */
constexpr uint64_t ACQUIRE_EVERY_TICK_NS = 1000000000ull;
constexpr uint64_t ACQUIRE_QUERY_INTERVAL_NS = 100000000ull;

/* How importing and mapping an attached GPU buffer went */
enum class gpu_mapping {
    MAPPED,     /* the whole buffer is mapped */
    GPU_FAILED, /* the import found the GPU failed */
    NO_MEMORY,  /* the mapping met ENOMEM, no memory or a GPU that failed after the import */
    REFUSED,    /* a buffer the protocol does not take, or stlxdm out of memory */
};

/* How the GPU compositor's import of an attached window buffer went */
enum class gpu_import {
    IMPORTED,
    GPU_FAILED, /* the import met no memory, and the probe context found the GPU failed */
    NO_MEMORY,  /* the import met no memory on a live GPU */
    REFUSED,    /* a buffer the GPU compositor cannot read */
};

static dm_window* find_window(dm_client& c, uint32_t win_id) {
    for (auto& w : c.windows) {
        if (w->win_id == win_id) {
            return w.get();
        }
    }

    return nullptr;
}

static bool is_dismissed(const dm_client& c, uint32_t win_id) {
    for (uint32_t id : c.dismissed_windows) {
        if (id == win_id) {
            return true;
        }
    }

    return false;
}

static dm_buffer* find_buffer(dm_window& w, uint32_t buf_id) {
    for (auto& b : w.buffers) {
        if (b.buf_id == buf_id) {
            return &b;
        }
    }

    return nullptr;
}

static dm_buffer* find_dismissed_buffer(dm_client& c, uint32_t win_id, uint32_t buf_id) {
    for (auto& d : c.dismissed_buffers) {
        if (d.win_id == win_id && d.buffer.buf_id == buf_id) {
            return &d.buffer;
        }
    }

    return nullptr;
}

static uint32_t count_gpu_buffers(const dm_client& c) {
    uint32_t count = static_cast<uint32_t>(c.dismissed_buffers.size());
    for (const auto& w : c.windows) {
        for (const auto& b : w->buffers) {
            count += b.is_gpu ? 1 : 0;
        }
    }

    return count;
}

static uint32_t count_shm_buffers(const dm_client& c) {
    uint32_t count = 0;
    for (const auto& w : c.windows) {
        for (const auto& b : w->buffers) {
            count += b.is_gpu ? 0 : 1;
        }
    }

    return count;
}

static bool is_held(const dm_window& w, size_t index) {
    int32_t i = static_cast<int32_t>(index);
    return i == w.current || i == w.pending;
}

/* Closes all the buffer holds but its release timeline, which a release-only record keeps. The import goes first, and
 * the mapping before the handles it came from */
static void close_reads(dm_buffer& b) {
    b.import.reset();
    b.gpu_address = 0;

    if (b.mapping) {
        munmap(b.mapping, b.size);
        b.mapping = nullptr;
        b.pixels = nullptr;
    }

    for (int* handle : { &b.shm, &b.gpu_buffer, &b.acquire }) {
        if (*handle >= 0) {
            close(*handle);
            *handle = -1;
        }
    }
}

static void close_buffer(dm_buffer& b) {
    close_reads(b);
    if (b.release >= 0) {
        close(b.release);
        b.release = -1;
    }
}

static void drop_dismissed_buffer(dm_client& c, uint32_t win_id, uint32_t buf_id) {
    for (size_t i = 0; i < c.dismissed_buffers.size(); i++) {
        dm_dismissed_buffer& d = c.dismissed_buffers[i];
        if (d.win_id == win_id && d.buffer.buf_id == buf_id) {
            close_buffer(d.buffer);
            c.dismissed_buffers.erase(c.dismissed_buffers.begin() + static_cast<long>(i));
            return;
        }
    }
}

static void drop_dismissed_window_buffers(dm_client& c, uint32_t win_id) {
    for (size_t i = c.dismissed_buffers.size(); i-- > 0;) {
        if (c.dismissed_buffers[i].win_id == win_id) {
            close_buffer(c.dismissed_buffers[i].buffer);
            c.dismissed_buffers.erase(c.dismissed_buffers.begin() + static_cast<long>(i));
        }
    }
}

static void remove_buffer(dm_window& w, size_t index) {
    close_buffer(w.buffers[index]);
    w.buffers.erase(w.buffers.begin() + static_cast<long>(index));

    int32_t removed = static_cast<int32_t>(index);
    for (int32_t* slot : { &w.current, &w.pending }) {
        if (*slot == removed) {
            *slot = -1;
        } else if (*slot > removed) {
            (*slot)--;
        }
    }
}

/* An attach takes the oldest three queued handles: the buffer, then
 * the acquire and release timelines */
static bool take_gpu_handles(dm_client& c, int (&handles)[SWP_GPU_HANDLES]) {
    if (c.queued_handle_count < SWP_GPU_HANDLES) {
        return false;
    }

    memcpy(handles, c.queued_handles, sizeof(handles));
    c.queued_handle_count -= SWP_GPU_HANDLES;
    memmove(c.queued_handles, c.queued_handles + SWP_GPU_HANDLES, c.queued_handle_count * sizeof(int));

    return true;
}

static bool is_valid_gpu_picture(const swp_attach_gpu_buffer& m) {
    bool width_fits = m.width >= 1 && m.width <= SWP_GPU_DIMENSION_MAX;
    bool height_fits = m.height >= 1 && m.height <= SWP_GPU_DIMENSION_MAX;
    if (!width_fits || !height_fits) {
        return false;
    }

    /* Composition offsets the picture's rows in 32 bits */
    bool rows_fit = static_cast<uint64_t>(m.pitch) * m.height <= UINT32_MAX;
    return m.pitch % 4 == 0 && m.pitch >= m.width * 4 && rows_fit && m.format == SWP_FMT_XRGB8888 &&
           m.offset % 4 == 0;
}

/* Consumes the buffer handle, and keeps the import's handle in `b` with its placement in `placement`. The restrict
 * comes before the import, since an import would take a shared memory handle as a buffer */
static gpu_mapping map_gpu_buffer(
    int gpu_fd,
    int handle,
    const swp_attach_gpu_buffer& m,
    dm_buffer& b,
    uint32_t& placement
) {
    gpu_restrict_args narrowed = {};
    narrowed.handle = handle;
    narrowed.rights = GPU_RIGHT_READ;
    int rc = ioctl(gpu_fd, GPU_HANDLE_RESTRICT, &narrowed);
    close(handle);
    if (rc != 0) {
        return gpu_mapping::REFUSED;
    }

    gpu_buffer_args import = {};
    import.source = narrowed.restricted;
    rc = ioctl(gpu_fd, GPU_BUFFER_IMPORT, &import);
    int import_error = errno;
    close(narrowed.restricted);
    if (rc != 0) {
        return import_error == EIO ? gpu_mapping::GPU_FAILED : gpu_mapping::REFUSED;
    }

    uint64_t picture_bytes = static_cast<uint64_t>(m.pitch) * m.height;
    bool picture_fits = m.offset <= import.size && picture_bytes <= import.size - m.offset;
    if (!picture_fits || !(import.flags & GPU_BUFFER_MAPPABLE)) {
        close(import.buffer);
        return gpu_mapping::REFUSED;
    }

    void* mapping = mmap(nullptr, import.size, PROT_READ, MAP_SHARED, import.buffer, 0);
    if (mapping == MAP_FAILED) {
        int map_error = errno;
        close(import.buffer);
        return map_error == ENOMEM ? gpu_mapping::NO_MEMORY : gpu_mapping::REFUSED;
    }

    b.mapping = mapping;
    b.size = import.size;
    b.pixels = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapping) + m.offset);
    b.gpu_buffer = import.buffer;
    placement = import.placement;

    return gpu_mapping::MAPPED;
}

/* Imports the buffer of `b`'s kept handle, `bytes` in `placement`, whose picture starts `offset` bytes in */
static gpu_import import_for_gpu(
    gpu_compositor& compositor,
    dm_buffer& b,
    uint64_t bytes,
    uint32_t placement,
    uint64_t offset
) {
    switch (compositor.import_window_buffer(b.gpu_buffer, bytes, placement, b.import)) {
    case window_import_result::imported:
        b.gpu_address = b.import->address() + offset;
        return gpu_import::IMPORTED;
    case window_import_result::no_memory:
        return compositor.has_gpu_failed() ? gpu_import::GPU_FAILED : gpu_import::NO_MEMORY;
    case window_import_result::refused:
        break;
    }

    return gpu_import::REFUSED;
}

/* A commit must arrive at the displayed size or at the size of the
 * configure it acks, anything else is a client bug */
static bool commit_size_fits(const dm_window& w, const dm_buffer& b, uint32_t ack_serial) {
    if (w.current < 0) {
        return true;
    }

    const dm_buffer& cur = w.buffers[static_cast<size_t>(w.current)];
    bool ok = b.width == cur.width && b.height == cur.height;
    for (uint32_t i = 0; !ok && i < w.sent_conf_count; i++) {
        const dm_sent_conf& e = w.sent_confs[i];
        ok = e.serial == ack_serial && e.w == b.width && e.h == b.height;
    }

    return ok;
}

/* Acks are protocol state, recorded at receipt. Configures older than
 * the ack are history, the acked one stays for the latch. */
static void record_ack(dm_window& w, uint32_t ack_serial) {
    if (ack_serial > w.acked_serial) {
        w.acked_serial = ack_serial;
    }

    uint32_t keep = 0;
    for (uint32_t i = 0; i < w.sent_conf_count; i++) {
        if (w.sent_confs[i].serial >= w.acked_serial) {
            w.sent_confs[keep++] = w.sent_confs[i];
        }
    }

    w.sent_conf_count = keep;
}

/* A commit's damage joins that of the commits since the last latch.
 * No rects, or more than fit, damage the whole buffer. */
static void add_commit_damage(dm_window& w, const swp_rect* rects, uint32_t count) {
    bool fits = count > 0 && count <= SWP_COMMIT_MAX_RECTS - w.pending_damage_count;
    if (w.pending_damage_whole || !fits) {
        w.pending_damage_whole = true;
        return;
    }

    memcpy(&w.pending_damage[w.pending_damage_count], rects, count * sizeof(swp_rect));
    w.pending_damage_count += count;
}

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

static bool is_latch_due(const dm_window& w, uint64_t now) {
    if (!w.buffers[static_cast<size_t>(w.pending)].is_gpu) {
        return true;
    }

    return now < w.pending_since_ns + ACQUIRE_EVERY_TICK_NS || now >= w.next_query_ns;
}

void server::handle_create_window(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_create_window*>(payload);
    if (m->width == 0 || m->height == 0 || find_window(c, m->win_id)) {
        c.dead = true;
        return;
    }

    auto w = std::make_unique<dm_window>();
    w->win_id = m->win_id;
    w->parent_id = m->parent_id;
    w->flags = m->flags;
    w->popup_flags = m->popup_flags;
    memcpy(w->title, m->title, sizeof(w->title));
    w->title[sizeof(w->title) - 1] = '\0';

    /* Popups sit relative to their parent, toplevels cascade */
    if (m->parent_id != 0) {
        dm_window* parent = find_window(c, m->parent_id);
        if (!parent) {
            c.dead = true;
            return;
        }

        w->x = parent->x + m->rel_x;
        w->y = parent->y + m->rel_y;
        w->output = toplevel_of(c, *parent).output;
    } else {
        int32_t under_pointer = m_layout.output_at(m_cursor_x, m_cursor_y);
        w->output = under_pointer >= 0 ? static_cast<uint32_t>(under_pointer) : m_layout.main_output();

        const damage_list::rect& place = m_layout.find(w->output)->rect;
        int32_t step = CASCADE_STEP
                     * static_cast<int32_t>(m_window_count[w->output] % CASCADE_WRAP);
        w->x = place.x + CASCADE_X + step;
        w->y = place.y + CASCADE_Y + step;
    }

    m_window_count[w->output]++;
    c.windows.push_back(std::move(w));
}

/* EINVAL means the timeline is at or past the point already */
void server::signal_release(dm_client& c, const dm_buffer& b, uint64_t point) {
    gpu_timeline_args signal = {};
    signal.timeline = b.release;
    signal.value = point;
    if (ioctl(m_gpu_fd, GPU_TIMELINE_SIGNAL, &signal) == 0 || errno == EINVAL) {
        return;
    }

    if (errno == EBADF) {
        c.dead = true;
        return;
    }

    printf("stlxdm: signaling a release timeline failed, errno %d, the point counts as reached\r\n", errno);
}

void server::release_picture(dm_client& c, uint32_t win_id, const dm_buffer& b) {
    if (b.is_gpu) {
        signal_release(c, b, b.release_point);
        return;
    }

    swp_release rel = { win_id, b.buf_id };
    send_to(c, SWP_MSG_RELEASE, &rel, sizeof(rel));
}

void server::stop_taking_gpu_buffers() {
    if (m_gpu_info.flags == 0) {
        return;
    }

    printf("stlxdm: the GPU failed, GPU buffers attached after it are released unshown\r\n");
    m_gpu_info.flags = 0;
    for (auto& c : m_clients) {
        if (c->hello_done) {
            send_to(*c, SWP_MSG_GPU_INFO, &m_gpu_info, sizeof(m_gpu_info));
        }
    }
}

void server::switch_to_cpu_on_failed_gpu() {
    stop_taking_gpu_buffers();
    if (composes_on_gpu()) {
        switch_to_cpu("the GPU failed");
    }
}

/* Imports a shared memory buffer for the GPU compositor. On a failed GPU it switches to the CPU, which reads the
 * buffer's mapping. False when its client must go */
bool server::import_shared_memory(dm_buffer& b) {
    gpu_buffer_args buffer_args = {};
    buffer_args.source = b.shm;
    if (ioctl(m_gpu_fd, GPU_BUFFER_IMPORT, &buffer_args) != 0) {
        if (errno != EIO) {
            return false;
        }

        switch_to_cpu_on_failed_gpu();
        return true;
    }

    b.gpu_buffer = buffer_args.buffer;

    /* The object may have shrunk since the mapping, and a compose offsets rows in 32 bits */
    if (buffer_args.size < b.size || b.size > UINT32_MAX) {
        return false;
    }

    switch (import_for_gpu(*m_gpu_compositor, b, buffer_args.size, buffer_args.placement, 0)) {
    case gpu_import::IMPORTED:
        return true;
    case gpu_import::GPU_FAILED:
        switch_to_cpu_on_failed_gpu();

        /* The switch closed every recorded buffer's import, and `b` is not recorded yet */
        if (!m_gpu_composes) {
            close(b.gpu_buffer);
            b.gpu_buffer = -1;
        }

        return true;
    case gpu_import::NO_MEMORY:
    case gpu_import::REFUSED:
        break;
    }

    return false;
}

/* A GPU picture the buffer shows or holds pending is released before
 * the drop while its client stays connected, as it may still wait */
void server::drop_buffer(dm_client& c, dm_window& w, size_t index) {
    const dm_buffer& b = w.buffers[index];
    if (b.is_gpu && is_held(w, index) && !c.dead) {
        signal_release(c, b, b.release_point);
    }

    remove_buffer(w, index);
}

/* Commits to a dismissed window may still be in flight, so the buffer
 * stays a release-only record until detached or its window destroyed */
void server::keep_release_only_record(dm_client& c, dm_window& w, size_t index) {
    dm_buffer& b = w.buffers[index];
    if (is_held(w, index) && !c.dead) {
        signal_release(c, b, b.release_point);
    }

    close_reads(b);
    b.is_release_only = true;
    c.dismissed_buffers.push_back({ w.win_id, std::move(b) });

    b.release = -1;
    remove_buffer(w, index);
}

void server::destroy_window_tree(dm_client& c, uint32_t win_id) {
    /* Children go first, matching the client library's cascade */
    for (size_t i = c.windows.size(); i-- > 0;) {
        if (c.windows[i]->parent_id == win_id) {
            destroy_window_tree(c, c.windows[i]->win_id);
        }
    }

    for (size_t i = 0; i < c.windows.size(); i++) {
        if (c.windows[i]->win_id != win_id) {
            continue;
        }

        dm_window* w = c.windows[i].get();
        scene_damage_window(w);

        forget_window(w);
        bool keeps_records = is_dismissed(c, win_id);
        while (!w->buffers.empty()) {
            size_t last = w->buffers.size() - 1;
            const dm_buffer& b = w->buffers[last];
            if (keeps_records && b.is_gpu && !b.is_detached) {
                keep_release_only_record(c, *w, last);
            } else {
                drop_buffer(c, *w, last);
            }
        }
        c.windows.erase(c.windows.begin() + static_cast<long>(i));
        return;
    }
}

void server::dismiss_window_tree(dm_client& c, uint32_t win_id) {
    note_dismissed_tree(c, win_id);
    destroy_window_tree(c, win_id);
}

/* The client learns of a dismissal after the fact, so the ids stay known
 * and the buffers it handed over go back to it as if superseded */
void server::note_dismissed_tree(dm_client& c, uint32_t win_id) {
    c.dismissed_windows.push_back(win_id);
    for (auto& w : c.windows) {
        if (w->parent_id == win_id) {
            note_dismissed_tree(c, w->win_id);
        } else if (w->win_id == win_id) {
            release_held_buffers(c, *w);
        }
    }
}

void server::release_held_buffers(dm_client& c, const dm_window& w) {
    for (int32_t held : { w.pending, w.current }) {
        if (held >= 0 && !w.buffers[static_cast<size_t>(held)].is_gpu) {
            swp_release rel = { w.win_id, w.buffers[static_cast<size_t>(held)].buf_id };
            send_to(c, SWP_MSG_RELEASE, &rel, sizeof(rel));
        }
    }
}

void server::handle_destroy_window(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_destroy_window*>(payload);
    std::erase(c.dismissed_windows, m->win_id);
    drop_dismissed_window_buffers(c, m->win_id);
    destroy_window_tree(c, m->win_id);
}

void server::handle_set_window(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_set_window*>(payload);
    dm_window* w = find_window(c, m->win_id);
    if (!w) {
        return;
    }

    switch (m->field) {
    case SWP_FIELD_TITLE:
        memcpy(w->title, m->title, sizeof(w->title));
        w->title[sizeof(w->title) - 1] = '\0';

        /* The bar repaints with the name it now carries */
        scene_damage_window(w);
        return;
    case SWP_FIELD_CURSOR:
        w->cursor = m->a;
        return;
    case SWP_FIELD_MIN_SIZE:
        w->min_w = m->a;
        w->min_h = m->b;
        return;
    case SWP_FIELD_MAX_SIZE:
        w->max_w = m->a;
        w->max_h = m->b;
        return;
    case SWP_FIELD_FULLSCREEN:
        /* Stored and honored once the fullscreen path lands */
        return;
    default:
        return;
    }
}

void server::handle_attach_buffer(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_attach_buffer*>(payload);
    dm_window* w = find_window(c, m->win_id);
    if (!w && !is_dismissed(c, m->win_id)) {
        c.dead = true;
        return;
    }

    if (m->width == 0 || m->height == 0 ||
        memchr(m->shm_name, '/', sizeof(m->shm_name)) ||
        (w && (find_buffer(*w, m->buf_id) || count_shm_buffers(c) == SWP_SHM_BUFFERS_MAX))) {
        c.dead = true;
        return;
    }

    char name[SWP_SHM_NAME_MAX];
    memcpy(name, m->shm_name, sizeof(name));
    name[sizeof(name) - 1] = '\0';

    char path[SWP_SHM_NAME_MAX + 16];
    snprintf(path, sizeof(path), "/dev/shm/%s", name);

    /* A dismissed window takes no buffer, but the name is still this
     * side's to unlink */
    if (!w) {
        unlink(path);
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        c.dead = true;
        return;
    }

    size_t size = static_cast<size_t>(m->width) * m->height * 4;
    void* pixels = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);

    /* The registry entry is unlinked as soon as both sides hold their
     * mappings, otherwise a crashed client pins the pages forever */
    unlink(path);
    if (pixels == MAP_FAILED) {
        close(fd);
        c.dead = true;
        return;
    }

    dm_buffer b;
    b.buf_id = m->buf_id;
    b.width = m->width;
    b.height = m->height;
    b.pitch = m->width * 4;

    b.mapping = pixels;
    b.size = size;
    b.pixels = static_cast<uint32_t*>(pixels);
    b.shm = fd;

    if (composes_on_gpu() && !import_shared_memory(b)) {
        close_buffer(b);
        c.dead = true;
        return;
    }

    w->buffers.push_back(std::move(b));
}

void server::handle_attach_gpu_buffer(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_attach_gpu_buffer*>(payload);
    int handles[SWP_GPU_HANDLES];
    if (!take_gpu_handles(c, handles)) {
        c.dead = true;
        return;
    }

    dm_window* w = find_window(c, m->win_id);
    bool is_dismissed_window = !w && is_dismissed(c, m->win_id);
    bool is_unused = w ? !find_buffer(*w, m->buf_id) : !find_dismissed_buffer(c, m->win_id, m->buf_id);
    bool may_attach = c.may_attach_gpu && (w || is_dismissed_window) && is_unused &&
                      count_gpu_buffers(c) < SWP_GPU_BUFFERS_MAX && is_valid_gpu_picture(*m);
    if (!may_attach) {
        for (int handle : handles) {
            close(handle);
        }

        c.dead = true;
        return;
    }

    dm_buffer b;
    b.buf_id = m->buf_id;
    b.width = m->width;
    b.height = m->height;
    b.pitch = m->pitch;

    b.is_gpu = true;
    b.release = handles[2];
    if (is_dismissed_window) {
        close(handles[0]);
        close(handles[1]);
        b.is_release_only = true;
        c.dismissed_buffers.push_back({ m->win_id, std::move(b) });
        return;
    }

    bool is_release_only = false;
    uint32_t placement = 0;
    switch (map_gpu_buffer(m_gpu_fd, handles[0], *m, b, placement)) {
    case gpu_mapping::MAPPED:
        break;
    case gpu_mapping::GPU_FAILED:
        switch_to_cpu_on_failed_gpu();
        is_release_only = true;
        break;
    case gpu_mapping::NO_MEMORY:
        printf("stlxdm: no memory or a failed GPU for a GPU buffer's mapping, its commits are released unshown\r\n");
        is_release_only = true;
        break;
    case gpu_mapping::REFUSED:
        close(handles[1]);
        close(handles[2]);
        c.dead = true;
        return;
    }

    if (!is_release_only && composes_on_gpu()) {
        switch (import_for_gpu(*m_gpu_compositor, b, b.size, placement, m->offset)) {
        case gpu_import::IMPORTED:
            break;
        case gpu_import::GPU_FAILED:
            switch_to_cpu_on_failed_gpu();
            is_release_only = true;
            break;
        case gpu_import::NO_MEMORY:
            printf("stlxdm: no memory for a GPU buffer's import, its commits are released unshown\r\n");
            is_release_only = true;
            break;
        case gpu_import::REFUSED:
            close(handles[1]);
            close_buffer(b);
            c.dead = true;
            return;
        }
    }

    if (is_release_only) {
        close(handles[1]);
        close_reads(b);
        b.is_release_only = true;
        w->buffers.push_back(std::move(b));
        return;
    }

    gpu_timeline_args query = {};
    query.timeline = handles[1];
    if (ioctl(m_gpu_fd, GPU_TIMELINE_QUERY, &query) != 0) {
        close(handles[1]);
        close_buffer(b);
        c.dead = true;
        return;
    }

    b.acquire = handles[1];
    w->buffers.push_back(std::move(b));
}

void server::handle_detach_buffer(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_detach_buffer*>(payload);
    dm_window* w = find_window(c, m->win_id);
    if (!w) {
        drop_dismissed_buffer(c, m->win_id, m->buf_id);
        return;
    }

    for (size_t i = 0; i < w->buffers.size(); i++) {
        dm_buffer& b = w->buffers[i];
        if (b.buf_id != m->buf_id) {
            continue;
        }

        /* A shared memory buffer detached while held is a client bug */
        if (!b.is_gpu && is_held(*w, i)) {
            c.dead = true;
            return;
        }

        /* Vulkan detaches a shown image before its successor presents */
        if (static_cast<int32_t>(i) == w->current && b.is_gpu) {
            b.is_detached = true;
            return;
        }

        drop_buffer(c, *w, i);
        return;
    }
}

void server::handle_commit(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_commit*>(payload);
    dm_window* w = find_window(c, m->win_id);
    if (!w) {
        if (is_dismissed(c, m->win_id)) {
            swp_release rel = { m->win_id, m->buf_id };
            send_to(c, SWP_MSG_RELEASE, &rel, sizeof(rel));
        } else {
            c.dead = true;
        }

        return;
    }

    dm_buffer* b = find_buffer(*w, m->buf_id);
    if (!b || b->is_gpu || !commit_size_fits(*w, *b, m->ack_serial)) {
        c.dead = true;
        return;
    }

    record_ack(*w, m->ack_serial);
    replace_pending(c, *w, static_cast<int32_t>(b - w->buffers.data()));
    add_commit_damage(*w, m->damage, m->n_damage);
}

void server::handle_commit_gpu(dm_client& c, const uint8_t* payload) {
    const auto* m = reinterpret_cast<const swp_commit_gpu*>(payload);
    dm_window* w = find_window(c, m->win_id);
    dm_buffer* b = w ? find_buffer(*w, m->buf_id) : find_dismissed_buffer(c, m->win_id, m->buf_id);
    if (!b || !b->is_gpu || b->is_detached || m->commit_flags != 0 || m->reserved != 0) {
        c.dead = true;
        return;
    }

    bool points_rise = m->release_point > b->release_point &&
                       (m->latch_point == 0 ||
                        (m->latch_point > b->release_point && m->latch_point < m->release_point));
    bool is_free = !w || !is_held(*w, static_cast<size_t>(b - w->buffers.data()));
    if (!points_rise || !is_free || (w && !commit_size_fits(*w, *b, m->ack_serial))) {
        c.dead = true;
        return;
    }

    b->acquire_point = m->acquire_point;
    b->latch_point = m->latch_point;
    b->release_point = m->release_point;
    if (w) {
        record_ack(*w, m->ack_serial);
        add_commit_damage(*w, m->damage, m->n_damage);
    }

    if (!w || b->is_release_only) {
        signal_release(c, *b, b->release_point);
        return;
    }

    replace_pending(c, *w, static_cast<int32_t>(b - w->buffers.data()));
    uint64_t now = now_ns();
    w->pending_since_ns = now;
    w->next_query_ns = now + ACQUIRE_EVERY_TICK_NS;
}

/* Only the latest commit counts, a superseded one is released without
 * ever reaching the screen */
void server::replace_pending(dm_client& c, dm_window& w, int32_t index) {
    if (w.pending >= 0) {
        release_picture(c, w.win_id, w.buffers[static_cast<size_t>(w.pending)]);
    }

    w.pending = index;
}

bool server::has_latch_work(uint32_t output) const {
    uint64_t now = now_ns();
    for (const auto& c : m_clients) {
        for (const auto& w : c->windows) {
            if (w->pending >= 0 && is_latch_due(*w, now) && latch_output(*c, *w) == output) {
                return true;
            }
        }
    }

    return false;
}

/* The toplevel whose tree holds `w`, `w` itself when it is one */
const dm_window& server::toplevel_of(const dm_client& c, const dm_window& w) const {
    const dm_window* top = &w;
    for (size_t depth = 0; top->parent_id != 0 && depth < c.windows.size(); depth++) {
        const dm_window* parent = nullptr;
        for (const auto& win : c.windows) {
            if (win->win_id == top->parent_id) {
                parent = win.get();
                break;
            }
        }

        if (!parent) {
            break;
        }

        top = parent;
    }

    return *top;
}

/* The output whose composes latch `w`'s commits: the first served of its
 * toplevel's output, the main one and the lowest served one, else the main one */
uint32_t server::latch_output(const dm_client& c, const dm_window& w) const {
    const dm_window& top = toplevel_of(c, w);
    if (m_outputs[top.output].pres) {
        return top.output;
    }

    uint32_t main = m_layout.main_output();
    if (m_outputs[main].pres) {
        return main;
    }

    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        if (m_outputs[output].pres) {
            return output;
        }
    }

    return main;
}

/* A frame on no member keeps the output it had */
void server::update_window_output(dm_window& w) {
    if (w.parent_id != 0 || w.current < 0) {
        return;
    }

    const dm_buffer& b = w.buffers[static_cast<size_t>(w.current)];
    damage_list::rect frame = decor::frame_rect(w, w.x, w.y, static_cast<int32_t>(b.width),
                                                static_cast<int32_t>(b.height));
    int32_t output = m_layout.output_holding(frame, nullptr);
    if (output >= 0) {
        w.output = static_cast<uint32_t>(output);
    }
}

int64_t server::latch_timeout_ns(uint64_t now) const {
    int64_t timeout = -1;
    for (const auto& c : m_clients) {
        for (const auto& w : c->windows) {
            if (w->pending < 0 || is_latch_due(*w, now)) {
                continue;
            }

            int64_t wait = static_cast<int64_t>(w->next_query_ns - now);
            if (timeout < 0 || wait < timeout) {
                timeout = wait;
            }
        }
    }

    return timeout;
}

/* Whether the window's pending GPU commit may latch, releasing it
 * unshown when its acquire point came with GPU_TIMELINE_FAILED */
bool server::is_acquire_reached(dm_client& c, dm_window& w, uint64_t now) {
    dm_buffer& b = w.buffers[static_cast<size_t>(w.pending)];
    gpu_timeline_args query = {};
    query.timeline = b.acquire;
    bool reached = ioctl(m_gpu_fd, GPU_TIMELINE_QUERY, &query) == 0 && query.value >= b.acquire_point;
    if (!reached) {
        if (now >= w.pending_since_ns + ACQUIRE_EVERY_TICK_NS) {
            w.next_query_ns = now + ACQUIRE_QUERY_INTERVAL_NS;
        }

        return false;
    }

    if (query.flags & GPU_TIMELINE_FAILED) {
        signal_release(c, b, b.release_point);
        w.pending = -1;
        return false;
    }

    return true;
}

bool server::has_configure_work() const {
    for (const auto& c : m_clients) {
        for (const auto& w : c->windows) {
            if (w->target_w != 0 && w->sent_conf_count < 2) {
                return true;
            }
        }
    }

    return false;
}

/* Sends at most one CONFIGURE per window per tick and none while two
 * are unacked, so a slow client skips straight to the newest size
 * instead of chewing through every intermediate one. */
void server::flush_configures() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->target_w == 0 || w->sent_conf_count >= 2) {
                continue;
            }

            /* The size the client already settles at without a send */
            uint32_t ref_w = 0;
            uint32_t ref_h = 0;
            if (w->sent_conf_count > 0) {
                ref_w = w->sent_confs[w->sent_conf_count - 1].w;
                ref_h = w->sent_confs[w->sent_conf_count - 1].h;
            } else if (w->current >= 0) {
                ref_w = w->buffers[static_cast<size_t>(w->current)].width;
                ref_h = w->buffers[static_cast<size_t>(w->current)].height;
            }

            if (w->target_w == ref_w && w->target_h == ref_h) {
                w->target_w = 0;
                w->target_h = 0;
                continue;
            }

            uint32_t serial = ++w->sent_serial;
            w->sent_confs[w->sent_conf_count++] = {
                serial, w->target_w, w->target_h,
                w->target_x, w->target_y
            };

            swp_configure msg = {
                w->win_id, w->target_w, w->target_h, serial,
                w.get() == m_focus ? SWP_STATE_FOCUSED : 0u
            };
            send_to(*c, SWP_MSG_CONFIGURE, &msg, sizeof(msg));

            w->target_w = 0;
            w->target_h = 0;
        }
    }
}

/* Latch a window's pending commit and translate its damage to desktop
 * coordinates: the whole window on map or resize, the commit rects otherwise. */
void server::latch_window(dm_client& c, dm_window& w) {
    const dm_buffer& nb = w.buffers[static_cast<size_t>(w.pending)];
    bool first_map = !w.mapped;
    bool resized = false;

    /* A commit from a client that received the timed input counts as
     * its response, whichever of its windows repainted */
    for (const auto& win : c.windows) {
        m_perf.answer(win->event_seq);
    }

    /* Clients may rely on the latch point coming before the replaced
     * picture's release and FRAME_DONE */
    if (nb.is_gpu && nb.latch_point != 0) {
        signal_release(c, nb, nb.latch_point);
    }

    if (w.current >= 0) {
        const dm_buffer& ob = w.buffers[static_cast<size_t>(w.current)];
        resized = ob.width != nb.width || ob.height != nb.height;
        if (resized) {
            scene_damage_window(&w);
        }

        release_picture(c, w.win_id, ob);
    }

    /* A resize latch moves the window to the spot its configure
     * promised, so an anchored edge stays visually fixed. The acked
     * entry is consumed here. */
    if (resized) {
        for (uint32_t i = 0; i < w.sent_conf_count; i++) {
            const dm_sent_conf& e = w.sent_confs[i];
            if (e.serial == w.acked_serial &&
                e.w == nb.width && e.h == nb.height) {
                w.x = e.x;
                w.y = e.y;
                break;
            }
        }
    }
    uint32_t keep = 0;
    for (uint32_t i = 0; i < w.sent_conf_count; i++) {
        if (w.sent_confs[i].serial > w.acked_serial) {
            w.sent_confs[keep++] = w.sent_confs[i];
        }
    }
    w.sent_conf_count = keep;

    if (first_map || resized) {
        damage_list::rect nr = { w.x - decor::BORDER - 1,
                                 w.y - decor::TITLE_H - 1,
                                 static_cast<int32_t>(nb.width) + 2 * decor::BORDER + 2,
                                 static_cast<int32_t>(nb.height) + decor::TITLE_H
                                     + decor::BORDER + 2 };
        if (w.flags & SWP_WF_BORDERLESS) {
            nr = { w.x, w.y, static_cast<int32_t>(nb.width), static_cast<int32_t>(nb.height) };
        }
        m_damage.add(nr.x, nr.y, nr.w, nr.h);
    } else if (w.pending_damage_whole || w.pending_damage_count == 0) {
        m_damage.add(w.x, w.y, static_cast<int32_t>(nb.width), static_cast<int32_t>(nb.height));
    } else {
        for (uint32_t i = 0; i < w.pending_damage_count; i++) {
            const swp_rect& r = w.pending_damage[i];
            m_damage.add(w.x + r.x, w.y + r.y, r.w, r.h);
        }
    }

    w.pending_damage_count = 0;
    w.pending_damage_whole = false;

    int32_t replaced = w.current;
    w.current = w.pending;
    w.pending = -1;
    if (first_map || resized) {
        update_window_output(w);
    }

    if (replaced >= 0 && w.buffers[static_cast<size_t>(replaced)].is_detached) {
        drop_buffer(c, w, static_cast<size_t>(replaced));
    }

    if (first_map) {
        w.mapped = true;
        scene_map(&w);
    }

    swp_frame_done done = { w.win_id, 0, now_ns() };
    send_to(c, SWP_MSG_FRAME_DONE, &done, sizeof(done));
}

/* Splits every piece by the hole, dropping fully covered parts.
 * Returns false once the piece list would exceed its cap, and the
 * caller stops subtracting from there. */
static bool subtract_hole(std::vector<damage_list::rect>& pieces,
                          const damage_list::rect& hole) {
    std::vector<damage_list::rect> out;
    out.reserve(pieces.size() + 4);

    for (const damage_list::rect& p : pieces) {
        int32_t hx0 = hole.x > p.x ? hole.x : p.x;
        int32_t hy0 = hole.y > p.y ? hole.y : p.y;
        int32_t hx1 = hole.x + hole.w < p.x + p.w
                    ? hole.x + hole.w : p.x + p.w;
        int32_t hy1 = hole.y + hole.h < p.y + p.h
                    ? hole.y + hole.h : p.y + p.h;
        if (hx0 >= hx1 || hy0 >= hy1) {
            out.push_back(p);
            continue;
        }

        /* Up to four bands survive around the covered middle */
        if (hy0 > p.y) {
            out.push_back({ p.x, p.y, p.w, hy0 - p.y });
        }
        if (hy1 < p.y + p.h) {
            out.push_back({ p.x, hy1, p.w, p.y + p.h - hy1 });
        }
        if (hx0 > p.x) {
            out.push_back({ p.x, hy0, hx0 - p.x, hy1 - hy0 });
        }
        if (hx1 < p.x + p.w) {
            out.push_back({ hx1, hy0, p.x + p.w - hx1, hy1 - hy0 });
        }
    }

    if (out.size() > OCCLUDE_PIECE_CAP) {
        return false;
    }

    pieces = std::move(out);
    return true;
}

decor::chrome_state server::chrome_of(const dm_window& w) const {
    decor::chrome_state st;
    st.focused = &w == m_focus;
    st.dragging = &w == m_drag;
    st.close_hover = &w == m_close_hover;
    st.close_pressed = &w == m_close_press;

    return st;
}

/* Compose one rect of `out` in desktop coordinates, bottom-up, each layer
 * clipped to the damage minus the opaque content of every window above it */
void server::compose_rect(dm_output& out, stlxgfx_surface_t* back,
                          const damage_list::rect& r) {
    /* Walk top-down recording what each window may paint, then
     * subtract its opaque content from everything beneath */
    std::vector<damage_list::rect> pieces = { r };
    std::vector<std::vector<damage_list::rect>> visible(m_zorder.size());
    bool subtracting = true;

    for (size_t i = m_zorder.size(); i-- > 0;) {
        dm_window* w = m_zorder[i];
        if (w->current < 0) {
            continue;
        }

        visible[i] = pieces;
        if (!subtracting) {
            continue;
        }

        const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];
        int32_t bw = static_cast<int32_t>(b.width);
        int32_t bh = static_cast<int32_t>(b.height);

        /* The bottom corner squares stay exposed, the rounded content
         * blit blends them over whatever lies beneath */
        int32_t r = decor::decorated(*w) ? decor::CONTENT_R : 0;
        if (r > 0 && bh > r && bw > 2 * r) {
            subtracting = subtract_hole(pieces, { w->x, w->y, bw, bh - r }) &&
                          subtract_hole(pieces, { w->x + r, w->y + bh - r,
                                                  bw - 2 * r, r });
        } else {
            subtracting = subtract_hole(pieces, { w->x, w->y, bw, bh });
        }
    }

    /* The backdrop, the main output's panels included, fills only what
     * stayed exposed */
    bool is_main = &out == &m_outputs[m_layout.main_output()];
    const stlxgfx_surface_t* backdrop = is_main ? m_backdrop : out.backdrop;
    const backdrop_picture* gpu_backdrop = is_main ? m_gpu_backdrop.get() : out.pictures.backdrop.get();
    if (gpu_backdrop) {
        backdrop = gpu_backdrop->surface();
    }

    int32_t ox = out.place.x;
    int32_t oy = out.place.y;
    for (const damage_list::rect& p : pieces) {
        if (backdrop) {
            stlxgfx_blit(back, p.x - ox, p.y - oy, backdrop, p.x - ox, p.y - oy,
                         static_cast<uint32_t>(p.w),
                         static_cast<uint32_t>(p.h));
        } else {
            stlxgfx_fill_rect(back, p.x - ox, p.y - oy, static_cast<uint32_t>(p.w),
                              static_cast<uint32_t>(p.h), m_conf->bg_color);
        }
    }

    decor::canvas canvas = { back, ox, oy };

    for (size_t i = 0; i < m_zorder.size(); i++) {
        dm_window* w = m_zorder[i];
        if (w->current < 0) {
            continue;
        }

        decor::chrome_state st = chrome_of(*w);

        /* The frame first, then the content with the frame's rounding
         * applied at its bottom corners */
        for (const damage_list::rect& p : visible[i]) {
            decor::draw(canvas, *w, st, p);
            decor::blit_content(canvas, *w, p);
        }
    }

    if (m_resize) {
        decor::draw_outline(canvas, m_outline, r);
    }

    /* Above the windows: the tooltip, star and overlay on the main output,
     * the overlay's dim on the others, then the pointer the frames draw */
    damage_list::rect local = { r.x - ox, r.y - oy, r.w, r.h };
    if (is_main) {
        m_panels->compose_top(back, local);
        m_power->draw_star(back, local);
        m_power->draw_overlay(back, local);
    } else {
        m_power->draw_dim(back, local);
    }

    if (out.cursor_in_frames) {
        m_cursor.draw(back, m_cursor_shape, m_cursor_x - ox, m_cursor_y - oy, local);
    }
}

static bool touches_any(const damage_list::rect& bounds, const std::vector<damage_list::rect>& rects) {
    for (const damage_list::rect& r : rects) {
        damage_list::rect common = damage_list::intersect(bounds, r);
        if (common.w > 0 && common.h > 0) {
            return true;
        }
    }

    return false;
}

static bool has_whole_output(const std::vector<damage_list::rect>& rects, int32_t width, int32_t height) {
    for (const damage_list::rect& r : rects) {
        if (r.x == 0 && r.y == 0 && r.w == width && r.h == height) {
            return true;
        }
    }

    return false;
}

static compose_target output_target(const presenter::target& t) {
    compose_target target;
    target.address = t.gpu_buffer->gpu_address();
    target.pitch = t.stride;
    target.scanout = t.gpu_buffer;

    return target;
}

/* Paints the panels' changes into the main output's backdrop and adds them
 * to the damage, noting the rows a GPU compose copies into its twin */
void server::flush_panels() {
    if (!m_gpu_backdrop) {
        m_panels->flush(m_damage);
        return;
    }

    damage_list painted;
    m_panels->flush(painted);
    if (painted.full()) {
        m_gpu_backdrop->mark_rows(0, static_cast<int32_t>(m_gpu_backdrop->surface()->height));
        m_damage.add_full();
        return;
    }

    for (uint32_t i = 0; i < painted.count(); i++) {
        const damage_list::rect& r = painted.at(i);
        m_gpu_backdrop->mark_rows(r.y, r.h);
        m_damage.add(r.x, r.y, r.w, r.h);
    }
}

/* Composes `effective` into the target `t` the CPU writes. False when its
 * surface could not be made */
bool server::compose_on_cpu(dm_output& out, const presenter::target& t, const damage_list& effective) {
    uint32_t sw = out.pres->width();
    uint32_t sh = out.pres->height();
    stlxgfx_surface_t* back = stlxgfx_surface_from_buffer(
        reinterpret_cast<uint8_t*>(t.pixels), sw, sh, t.stride, 32,
        16, 8, 0);
    if (!back) {
        return false;
    }

    if (effective.full()) {
        damage_list::rect whole = { out.place.x, out.place.y, static_cast<int32_t>(sw), static_cast<int32_t>(sh) };
        compose_rect(out, back, whole);
    } else {
        for (uint32_t i = 0; i < effective.count(); i++) {
            damage_list::rect r = effective.at(i);
            if (damage_list::clip(r, static_cast<int32_t>(sw), static_cast<int32_t>(sh))) {
                compose_rect(out, back, { r.x + out.place.x, r.y + out.place.y, r.w, r.h });
            }
        }
    }

    stlxgfx_destroy_surface(back);

    return true;
}

/* Composes `effective` into the output buffer `t` names, after capturing the collapse backdrop at its capture frame.
 * False when the frame's pictures or a compose failed, which takes every output's composes off the GPU */
bool server::compose_on_gpu(dm_output& out, const presenter::target& t, const damage_list& effective) {
    int32_t sw = static_cast<int32_t>(out.pres->width());
    int32_t sh = static_cast<int32_t>(out.pres->height());
    if (switch_to_cpu_on_misfit_backdrop(out)) {
        return false;
    }

    dm_output_pictures& pictures = out.pictures;
    std::vector<damage_list::rect>& rects = pictures.rects;
    rects.clear();
    if (effective.full()) {
        rects.push_back({ 0, 0, sw, sh });
    } else {
        for (uint32_t i = 0; i < effective.count(); i++) {
            damage_list::rect r = effective.at(i);
            if (damage_list::clip(r, sw, sh)) {
                rects.push_back(r);
            }
        }
    }

    bool is_main = &out == &m_outputs[m_layout.main_output()];
    bool captures_now = is_main && m_power->wants_capture() && has_whole_output(rects, sw, sh);
    if (captures_now && !compose_capture(out)) {
        return false;
    }

    if (!draw_pictures(out, rects, false)) {
        switch_to_cpu("a window's frame picture ran out of memory");
        return false;
    }

    compose_target target = output_target(t);
    target.readback = pictures.readback.get();
    compose_result result = m_gpu_compositor->compose(
        *pictures.compose,
        target,
        pictures.scene,
        rects,
        gpu_backdrop_of(out)
    );
    if (switch_to_cpu_on_failed_compose(result)) {
        return false;
    }

    if (target.readback) {
        compare_with_cpu(out, t.stride, false);
    }

    return true;
}

/* Composes the main output's frame beneath the orbs, its upper top picture holding the hint alone, into the collapse
 * backdrop, which the collapse frames fade. False when it took every output's composes off the GPU */
bool server::compose_capture(dm_output& out) {
    int32_t sw = static_cast<int32_t>(out.pres->width());
    int32_t sh = static_cast<int32_t>(out.pres->height());
    std::vector<damage_list::rect> whole = { { 0, 0, sw, sh } };
    layer_scene& scene = out.pictures.scene;
    if (!draw_pictures(out, whole, true)) {
        switch_to_cpu("a window's frame picture ran out of memory");
        return false;
    }

    compose_target target;
    target.address = m_gpu_capture->address();
    target.pitch = static_cast<uint32_t>(sw) * 4;
    compose_result result = m_gpu_compositor->compose(
        *out.pictures.compose,
        target,
        scene,
        whole,
        gpu_backdrop_of(out)
    );
    if (switch_to_cpu_on_failed_compose(result)) {
        return false;
    }

    m_power->mark_captured();

    return true;
}

/* Takes every output's composes off the GPU when `out` is a main output whose failed desktop rebuild left a backdrop
 * of another size, which the shader would read past. True when it took them off */
bool server::switch_to_cpu_on_misfit_backdrop(dm_output& out) {
    const stlxgfx_surface_t* backdrop = gpu_backdrop_of(out).surface();
    if (backdrop->width == out.pres->width() && backdrop->height == out.pres->height()) {
        return false;
    }

    switch_to_cpu("the main output's backdrop has another size");

    return true;
}

/* Takes every output's composes off the GPU unless `result` composed, after `stop_taking_gpu_buffers` when the
 * device was lost on a failed GPU. True when it took them off */
bool server::switch_to_cpu_on_failed_compose(compose_result result) {
    switch (result) {
    case compose_result::composed:
        return false;
    case compose_result::not_submitted:
        switch_to_cpu("a compose could not be submitted");
        break;
    case compose_result::lost:
        if (m_gpu_compositor->has_gpu_failed()) {
            switch_to_cpu_on_failed_gpu();
        } else {
            switch_to_cpu("the compositor's GPU context was lost");
        }

        break;
    case compose_result::unfinished:
        switch_to_cpu("a compose passed its deadline");
        break;
    }

    return true;
}

backdrop_picture& server::gpu_backdrop_of(dm_output& out) {
    bool is_main = &out == &m_outputs[m_layout.main_output()];
    return is_main ? *m_gpu_backdrop : *out.pictures.backdrop;
}

/* Draws the frames and top pictures a GPU compose of `out` reads in `rects`, in its pixels, and fills its scene with
 * what it shows, or what the collapse backdrop holds when `is_capture`. Needs `out`'s pictures. False without memory */
bool server::draw_pictures(dm_output& out, const std::vector<damage_list::rect>& rects, bool is_capture) {
    layer_scene& scene = out.pictures.scene;
    int32_t ox = out.place.x;
    int32_t oy = out.place.y;

    scene.width = static_cast<int32_t>(out.pres->width());
    scene.height = static_cast<int32_t>(out.pres->height());
    scene.backdrop = gpu_backdrop_of(out).twin_layer();
    scene.windows.clear();
    scene.content_color = decor::CONTENT_BG;

    for (dm_window* w : m_zorder) {
        if (w->current < 0) {
            continue;
        }

        damage_list::rect bounds = decor::bounds(*w);
        if (!touches_any({ bounds.x - ox, bounds.y - oy, bounds.w, bounds.h }, rects)) {
            continue;
        }

        const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];
        layer_window shown;
        shown.content = { w->x - ox, w->y - oy, static_cast<int32_t>(b.width), static_cast<int32_t>(b.height) };
        if (decor::decorated(*w)) {
            if (!w->frame.update(*m_gpu_compositor, *w, chrome_of(*w))) {
                return false;
            }

            shown.radius = decor::CONTENT_R;
            w->frame.place(*w, ox, oy, shown);
        }

        shown.has_picture = b.gpu_address != 0;
        shown.picture = { b.gpu_address, b.pitch, shown.content.x, shown.content.y };
        shown.import = b.import.get();
        scene.windows.push_back(shown);
    }

    draw_top_pictures(out, rects, is_capture);

    scene.lower_top = out.pictures.lower_top->layer();
    scene.is_dimmed = m_power->active();
    scene.dim_color = m_power->dim_color();
    scene.upper_top = out.pictures.upper_top->layer();

    return true;
}

/* Draws `compose_rect`'s draws above the windows, in its order, into the lower top picture before the power overlay's
 * dim and into the upper one after it, which for a capture holds only the hint drawn beneath the orbs */
void server::draw_top_pictures(dm_output& out, const std::vector<damage_list::rect>& rects, bool is_capture) {
    top_picture& lower = *out.pictures.lower_top;
    top_picture& upper = *out.pictures.upper_top;
    int32_t ox = out.place.x;
    int32_t oy = out.place.y;
    bool is_main = &out == &m_outputs[m_layout.main_output()];

    lower.begin();
    upper.begin();
    if (m_resize) {
        lower.add_bounds({ m_outline.x - ox, m_outline.y - oy, m_outline.w, m_outline.h });
    }

    if (is_main) {
        lower.add_bounds(m_panels->top_bounds());
        lower.add_bounds(m_power->star_bounds());
        upper.add_bounds(m_power->overlay_bounds());
    }

    bool draws_pointer = out.cursor_in_frames && !is_capture;
    if (draws_pointer) {
        damage_list::rect pointer = cursor_bounds();
        upper.add_bounds({ pointer.x - ox, pointer.y - oy, pointer.w, pointer.h });
    }

    decor::canvas lower_canvas = { lower.surface(), ox, oy };
    for (const damage_list::rect& r : rects) {
        lower.clear(r);
        if (m_resize) {
            decor::draw_outline(lower_canvas, m_outline, { r.x + ox, r.y + oy, r.w, r.h });
        }

        if (is_main) {
            m_panels->compose_top(lower.surface(), r);
            m_power->draw_star(lower.surface(), r);
        }

        upper.clear(r);
        if (is_main && is_capture) {
            m_power->draw_hint(upper.surface(), r);
        } else if (is_main) {
            m_power->draw_over_dim(upper.surface(), r);
        }

        if (draws_pointer) {
            m_cursor.draw(upper.surface(), m_cursor_shape, m_cursor_x - ox, m_cursor_y - oy, r);
        }
    }
}

/* One collapse frame on the main output, from the overlay's cached backdrop */
void server::compose_collapse(dm_output& out) {
    presenter::target t = out.pres->acquire();
    bool composed = t.gpu_buffer ? collapse_on_gpu(out, t) : collapse_on_cpu(out, t);
    if (composed) {
        damage_list full;
        full.add_full();
        out.pres->present(full);
        out.history[out.history_head] = full;
        out.history_head = (out.history_head + 1) % DM_DAMAGE_HISTORY;
    }

    out.damage.clear();
}

/* Draws a collapse frame into `back`, at the main output's size, and the pointer when the frames draw it */
void server::draw_collapse_frame(dm_output& out, stlxgfx_surface_t* back) {
    damage_list::rect whole = { 0, 0, static_cast<int32_t>(back->width),
                                static_cast<int32_t>(back->height) };
    m_power->draw_collapse(back);
    if (out.cursor_in_frames) {
        m_cursor.draw(back, m_cursor_shape, m_cursor_x - out.place.x, m_cursor_y - out.place.y, whole);
    }
}

/* Draws a collapse frame into the target `t` the CPU writes. False when its
 * surface could not be made */
bool server::collapse_on_cpu(dm_output& out, const presenter::target& t) {
    stlxgfx_surface_t* back = stlxgfx_surface_from_buffer(
        reinterpret_cast<uint8_t*>(t.pixels), out.pres->width(),
        out.pres->height(), t.stride, 32, 16, 8, 0);
    if (!back) {
        return false;
    }

    draw_collapse_frame(out, back);
    stlxgfx_destroy_surface(back);

    return true;
}

/* Composes a collapse frame into the output buffer `t` names, drawn into the upper top picture, which the faded
 * collapse backdrop covers opaquely. False when it took every output's composes off the GPU */
bool server::collapse_on_gpu(dm_output& out, const presenter::target& t) {
    if (switch_to_cpu_on_misfit_backdrop(out)) {
        return false;
    }

    int32_t sw = static_cast<int32_t>(out.pres->width());
    int32_t sh = static_cast<int32_t>(out.pres->height());
    damage_list::rect whole = { 0, 0, sw, sh };

    /* The faded collapse backdrop overwrites every pixel of the picture, so nothing is cleared beneath it */
    top_picture& upper = *out.pictures.upper_top;
    upper.begin();
    upper.add_bounds(whole);
    draw_collapse_frame(out, upper.surface());

    layer_scene& scene = out.pictures.scene;
    scene.width = sw;
    scene.height = sh;
    scene.backdrop = gpu_backdrop_of(out).twin_layer();
    scene.windows.clear();

    scene.lower_top = {};
    scene.is_dimmed = false;
    scene.upper_top = upper.layer();

    std::vector<damage_list::rect>& rects = out.pictures.rects;
    rects.assign(1, whole);
    compose_target target = output_target(t);
    target.readback = out.pictures.readback.get();
    compose_result result = m_gpu_compositor->compose(
        *out.pictures.compose,
        target,
        scene,
        rects,
        gpu_backdrop_of(out)
    );
    if (switch_to_cpu_on_failed_compose(result)) {
        return false;
    }

    if (target.readback) {
        compare_with_cpu(out, t.stride, true);
    }

    return true;
}

/* Draws the frame the GPU just composed into `out` again on the CPU, into its comparison frame, and logs where the
 * frame read back at `pitch` differs from it */
void server::compare_with_cpu(dm_output& out, uint32_t pitch, bool is_collapse) {
    dm_output_pictures& pictures = out.pictures;
    uint32_t width = out.pres->width();
    uint32_t height = out.pres->height();
    uint32_t frame_pitch = width * 4;
    stlxgfx_surface_t* frame = stlxgfx_surface_from_buffer(
        pictures.comparison->pixels(),
        width,
        height,
        frame_pitch,
        32,
        16,
        8,
        0
    );
    if (!frame) {
        return;
    }

    pictures.rounding.clear();
    if (is_collapse) {
        /* The faded collapse backdrop covers every pixel opaquely, so no pixel may round apart */
        draw_collapse_frame(out, frame);
    } else {
        /* Any capture of this frame was composed on the GPU before it, so these composes write none */
        for (const damage_list::rect& r : pictures.rects) {
            compose_rect(out, frame, { r.x + out.place.x, r.y + out.place.y, r.w, r.h });
        }

        rounding_regions(out);
    }

    stlxgfx_destroy_surface(frame);

    pictures.compared++;
    frame_difference difference = compare_frames(
        pictures.readback->pixels(),
        pitch,
        pictures.comparison->pixels(),
        frame_pitch,
        pictures.rects,
        pictures.rounding
    );
    if (difference.outside_count == 0 && difference.inside_count == 0) {
        return;
    }

    printf("stlxdm: GPU frame %llu of output %u differs from the CPU's in %u pixels outside the regions that may round "
           "apart and %u inside, the first at (%d, %d) of the output\r\n",
           static_cast<unsigned long long>(pictures.compared), static_cast<uint32_t>(&out - m_outputs),
           difference.outside_count, difference.inside_count, difference.first_x, difference.first_y);
}

/* Adds where `out`'s GPU frame may round apart from the CPU's to its rounding regions, in its pixels: glow rings, the
 * frames of windows whose content is no wider than two corner radii, and this compose's draws into both top pictures */
void server::rounding_regions(dm_output& out) {
    std::vector<damage_list::rect>& regions = out.pictures.rounding;
    int32_t ox = out.place.x;
    int32_t oy = out.place.y;
    for (const dm_window* w : m_zorder) {
        if (w->current < 0 || !decor::decorated(*w)) {
            continue;
        }

        const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];
        if (static_cast<int32_t>(b.width) <= 2 * decor::CONTENT_R) {
            damage_list::rect strips[decor::FRAME_STRIPS];
            decor::frame_strips(*w, strips);
            for (const damage_list::rect& strip : strips) {
                regions.push_back({ strip.x - ox, strip.y - oy, strip.w, strip.h });
            }

            continue;
        }

        damage_list::rect ring = decor::bounds(*w);
        ring.x -= ox;
        ring.y -= oy;

        regions.push_back({ ring.x, ring.y, ring.w, 1 });
        regions.push_back({ ring.x, ring.y + ring.h - 1, ring.w, 1 });
        regions.push_back({ ring.x, ring.y + 1, 1, ring.h - 2 });
        regions.push_back({ ring.x + ring.w - 1, ring.y + 1, 1, ring.h - 2 });
    }

    const std::vector<damage_list::rect>& lower = out.pictures.lower_top->drawn();
    const std::vector<damage_list::rect>& upper = out.pictures.upper_top->drawn();
    regions.insert(regions.end(), lower.begin(), lower.end());
    regions.insert(regions.end(), upper.begin(), upper.end());
}

/* Overlay phases: an activation edge repaints every output, as the others draw its dim, a transition the main output
 * and a winding hold the orb boxes */
void server::advance_power() {
    m_power->update();

    bool power_active = m_power->active();
    const damage_list::rect& main = m_layout.find(m_layout.main_output())->rect;
    if (power_active != m_power_was_active) {
        m_damage.add_full();
        m_power_was_active = power_active;
    } else if (m_power->transitioning()) {
        m_damage.add(main.x, main.y, main.w, main.h);
    } else if (m_power->animating()) {
        for (int32_t i = 0; i < 2; i++) {
            damage_list::rect ob = m_power->orb_box(i);
            m_damage.add(ob.x, ob.y, ob.w, ob.h);
        }
    }
}

void server::power_tick() {
    advance_power();
    m_power->run_action();
}

void server::compose_tick(uint32_t output) {
    dm_output& out = m_outputs[output];
    if (!out.pres) {
        return;
    }

    uint64_t now = now_ns();
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->pending < 0 || !is_latch_due(*w, now) || latch_output(*c, *w) != output) {
                continue;
            }

            bool is_gpu = w->buffers[static_cast<size_t>(w->pending)].is_gpu;
            if (!is_gpu || is_acquire_reached(*c, *w, now)) {
                latch_window(*c, *w);
            }
        }
    }

    flush_configures();

    bool is_main = output == m_layout.main_output();
    if (is_main) {
        flush_panels();
        advance_power();
    }

    spread_damage();

    /* Windows still latch and release their buffers and the power action still runs, so clients and the loop settle */
    if (m_gpu_stopped) {
        out.damage.clear();
        if (is_main) {
            m_power->run_action();
        }

        return;
    }

    /* Collapse frames fade the collapse backdrop under the contracting
     * light in place of the desktop's scene */
    if (is_main && m_power->collapsing()) {
        compose_collapse(out);
        return;
    }

    if (out.damage.empty()) {
        return;
    }

    presenter::target t = out.pres->acquire();

    /* A target older than one present is missing frames of content,
     * so their damage is unioned back in. Unknown age repaints fully. */
    damage_list effective = out.damage;
    if (t.age == 0 || t.age > DM_DAMAGE_HISTORY + 1) {
        effective.add_full();
    } else {
        for (uint32_t back = 1; back < t.age; back++) {
            uint32_t idx = (out.history_head + DM_DAMAGE_HISTORY - back)
                         % DM_DAMAGE_HISTORY;
            const damage_list& h = out.history[idx];
            if (h.full()) {
                effective.add_full();
                break;
            }

            for (uint32_t i = 0; i < h.count(); i++) {
                const damage_list::rect& r = h.at(i);
                effective.add(r.x, r.y, r.w, r.h);
            }
        }
    }

    bool composed = t.gpu_buffer ? compose_on_gpu(out, t, effective) : compose_on_cpu(out, t, effective);
    if (!composed) {
        out.damage.clear();
        return;
    }

    out.pres->present(effective);
    m_perf.note_present(now_ns());

    /* This frame's new damage joins the history for older targets */
    out.history[out.history_head] = out.damage;
    out.history_head = (out.history_head + 1) % DM_DAMAGE_HISTORY;
    out.damage.clear();

    /* Runs only after the collapse's final dark frame is on screen */
    if (is_main) {
        m_power->run_action();
    }
}
