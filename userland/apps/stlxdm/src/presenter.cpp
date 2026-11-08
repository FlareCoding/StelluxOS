#include "presenter.hpp"

#include "display.hpp"

#include <stlx/gfxfb_abi.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

constexpr uint32_t BYTES_PER_PIXEL = 4;

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

static gfxfb::gfxfb_rect to_gfxfb_rect(const damage_list::rect& r) {
    return { static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y),
             static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h) };
}

/* Copies rectangle `r`, which must lie on screen, from a composed frame
 * `composed_width` pixels wide into `dst`, whose rows are `dst_pitch` bytes apart */
static void copy_rect(uint8_t* dst, uint32_t dst_pitch, const uint8_t* composed,
                      uint32_t composed_width, const damage_list::rect& r) {
    for (int32_t row = r.y; row < r.y + r.h; row++) {
        memcpy(dst + static_cast<size_t>(row) * dst_pitch + static_cast<size_t>(r.x) * BYTES_PER_PIXEL,
               composed + (static_cast<size_t>(row) * composed_width + static_cast<size_t>(r.x)) * BYTES_PER_PIXEL,
               static_cast<size_t>(r.w) * BYTES_PER_PIXEL);
    }
}

/* Fills `primary` with the primary output's state. Returns 0, `-EAGAIN` when no output is
 * primary or the primary relights, or `GFXFB_GET_OUTPUT`'s negative errno after printing it */
static int read_lit_primary(int fd, gfxfb::gfxfb_output_info& primary) {
    uint32_t count = 1; /* enough to ask output 0, whose reply gives the real count */
    for (uint32_t i = 0; i < count; i++) {
        primary = {};
        primary.output = i;
        if (ioctl(fd, gfxfb::GFXFB_GET_OUTPUT, &primary) < 0) {
            int error = errno;
            printf("stlxdm: output state read failed: %s\r\n", strerror(error));
            return -error;
        }

        if ((primary.flags & gfxfb::GFXFB_OUTPUT_PRIMARY) != 0) {
            return (primary.flags & gfxfb::GFXFB_OUTPUT_LIT) != 0 ? 0 : -EAGAIN;
        }

        count = primary.output_count;
    }

    return -EAGAIN;
}

/* Whether `output` reads lit at `width` by `height`, or reading it fails */
static bool still_lit_at(int fd, uint32_t output, uint32_t width, uint32_t height) {
    gfxfb::gfxfb_output_info info = {};
    info.output = output;
    if (ioctl(fd, gfxfb::GFXFB_GET_OUTPUT, &info) < 0) {
        return true;
    }

    bool lit = (info.flags & gfxfb::GFXFB_OUTPUT_LIT) != 0;
    bool same_size = info.current.width == width && info.current.height == height;

    return lit && same_size;
}

int memcpy_presenter::init(int fd) {
    m_fd = fd;
    return update_output();
}

void memcpy_presenter::shutdown() {
    free(m_back);
    m_back = nullptr;

    if (m_scanout) {
        munmap(m_scanout, m_size);
        m_scanout = nullptr;
    }

    m_fd = -1;
}

presenter::target memcpy_presenter::acquire() {
    target t;
    t.pixels = reinterpret_cast<uint32_t*>(m_back);
    t.stride = m_width * 4;

    /* One persistent buffer: undefined on first use, then it always
     * holds the previous frame */
    t.age = m_first_acquire ? 0 : 1;
    m_first_acquire = false;

    return t;
}

void memcpy_presenter::flush_display(const damage_list::rect* rects, uint32_t count) {
    if (!m_needs_flush || count == 0) {
        return;
    }

    gfxfb::gfxfb_rect changed[damage_list::MAX_RECTS];
    uint32_t changed_count = 0;

    /* A failed flush left its regions unshown, so the next one sends the whole screen */
    if (m_last_flush_failed) {
        changed[changed_count++] = { 0, 0, m_width, m_height };
    } else {
        for (uint32_t i = 0; i < count; i++) {
            changed[changed_count++] = to_gfxfb_rect(rects[i]);
        }
    }

    gfxfb::gfxfb_flush_args args = {};
    args.rects = reinterpret_cast<uint64_t>(changed);
    args.count = changed_count;

    /* `EAGAIN` comes while no output is primary or the primary is dark or relights, which an output event
     * reports, so that refusal prints nothing */
    bool failed = ioctl(m_fd, gfxfb::GFXFB_FLUSH, &args) < 0;
    bool output_dark = failed && errno == EAGAIN;
    if (failed && !output_dark && !m_last_flush_failed) {
        printf("stlxdm: display flush failed: %s\r\n", strerror(errno));
    }

    m_last_flush_failed = failed;
}

void memcpy_presenter::present(const damage_list& damage) {
    if (m_output_changed) {
        return;
    }

    if (damage.full()) {
        damage_list::rect whole = { 0, 0, static_cast<int32_t>(m_width),
                                    static_cast<int32_t>(m_height) };
        copy_rect(m_scanout, m_pitch, m_back, m_width, whole);
        flush_display(&whole, 1);
        return;
    }

    damage_list::rect copied[damage_list::MAX_RECTS];
    uint32_t count = 0;

    for (uint32_t i = 0; i < damage.count(); i++) {
        damage_list::rect r = damage.at(i);
        if (damage_list::clip(r, static_cast<int32_t>(m_width), static_cast<int32_t>(m_height))) {
            copy_rect(m_scanout, m_pitch, m_back, m_width, r);
            copied[count++] = r;
        }
    }

    flush_display(copied, count);
}

/* Maps the pixels again before unmapping the earlier mapping, so a failure leaves the presenter as it was */
int memcpy_presenter::update_output() {
    m_output_changed = false;

    gfxfb::gfxfb_output_info primary;
    int rc = read_lit_primary(m_fd, primary);
    if (rc != 0) {
        return rc == -EAGAIN ? rc : -1;
    }

    gfxfb::gfxfb_info info;
    if (ioctl(m_fd, gfxfb::GFXFB_GET_INFO, &info) < 0) {
        if (errno == EAGAIN) {
            return -EAGAIN;
        }

        printf("stlxdm: framebuffer layout read failed: %s\r\n", strerror(errno));

        return -1;
    }

    void* mem = mmap(nullptr, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (mem == MAP_FAILED) {
        printf("stlxdm: framebuffer mapping failed: %s\r\n", strerror(errno));
        return -1;
    }

    uint32_t width = static_cast<uint32_t>(info.width);
    uint32_t height = static_cast<uint32_t>(info.height);
    bool resized = width != m_width || height != m_height;
    uint8_t* back = m_back;
    if (resized) {
        back = static_cast<uint8_t*>(malloc(static_cast<size_t>(width) * height * BYTES_PER_PIXEL));
    }

    if (!back) {
        printf("stlxdm: no memory for the composed frame\r\n");
        munmap(mem, info.size);
        return -1;
    }

    if (back != m_back) {
        free(m_back);
    }

    if (m_scanout) {
        munmap(m_scanout, m_size);
    }

    m_scanout = static_cast<uint8_t*>(mem);
    m_size = info.size;
    m_pitch = static_cast<uint32_t>(info.pitch);
    m_needs_flush = (info.flags & gfxfb::GFXFB_INFO_NEEDS_FLUSH) != 0;
    m_back = back;

    m_output = primary.output;
    m_width = width;
    m_height = height;
    m_refresh_millihertz = primary.current.refresh_millihertz;

    m_first_acquire = true;
    m_last_flush_failed = false;

    return 0;
}

void buffer_presenter::take_mode(display& owner, const gfxfb::gfxfb_output_info& info) {
    m_display = &owner;
    m_fd = owner.fd();

    m_output = info.output;
    m_width = info.current.width;
    m_height = info.current.height;
    m_refresh_millihertz = info.current.refresh_millihertz;
}

void buffer_presenter::take_cursor_limits(const gfxfb::gfxfb_output_info& info) {
    m_cursor_max_width = info.cursor_max_width;
    m_cursor_max_height = info.cursor_max_height;
}

void buffer_presenter::stop_cursor() {
    hide_cursor();
    m_cursor_max_width = 0;
    m_cursor_max_height = 0;
}

void buffer_presenter::wait_for_present() {
    while (m_present_pending) {
        m_display->take_events();
    }
}

void buffer_presenter::note_changed(uint32_t index, const damage_list& damage) {
    damage_list& unpresented = m_unpresented[index];
    if (damage.full()) {
        unpresented.add_full();
        return;
    }

    for (uint32_t i = 0; i < damage.count(); i++) {
        damage_list::rect r = damage.at(i);
        if (damage_list::clip(r, static_cast<int32_t>(m_width), static_cast<int32_t>(m_height))) {
            unpresented.add(r.x, r.y, r.w, r.h);
        }
    }
}

void buffer_presenter::present_back(uint32_t handle) {
    damage_list& changed = m_unpresented[m_back_index];
    if (!present_buffer(handle, changed)) {
        return;
    }

    changed.clear();
    m_present_pending = true;
    m_pending_frame_start_ns = m_frame_start_ns;
}

int flip_presenter::init(display& owner, const gfxfb::gfxfb_output_info& info) {
    take_mode(owner, info);

    int rc = create_buffers();
    if (rc != 0) {
        release_buffers();
        return rc;
    }

    take_cursor_limits(info);

    return 0;
}

/* Creates the two display buffers and the composed frame at `m_width` by `m_height`, each buffer's first present
 * naming all of it. Returns 0 or a negative errno, printing why for all but `-EBUSY` */
int flip_presenter::create_buffers() {
    for (display_buffer& buffer : m_buffers) {
        int rc = create_buffer(buffer);
        if (rc != 0) {
            return rc;
        }
    }

    m_composed = static_cast<uint8_t*>(
        malloc(static_cast<size_t>(m_width) * m_height * BYTES_PER_PIXEL));
    if (!m_composed) {
        printf("stlxdm: no memory for the composed frame of output %u\r\n", m_output);
        return -ENOMEM;
    }

    for (damage_list& unpresented : m_unpresented) {
        unpresented.add_full();
    }

    m_back_index = 0;
    m_first_acquire = true;

    return 0;
}

/* Creates one buffer at its output's mode and maps it. Returns 0 or a negative errno,
 * printing why for all but `-EBUSY`. A buffer that cannot be mapped is dropped */
int flip_presenter::create_buffer(display_buffer& buffer) {
    gfxfb::gfxfb_buffer_args args = {};
    args.width = m_width;
    args.height = m_height;
    args.format = gfxfb::GFXFB_FORMAT_XRGB8888;
    if (ioctl(m_fd, gfxfb::GFXFB_CREATE_BUFFER, &args) < 0) {
        int error = errno;
        if (error == ENOTTY) {
            printf("stlxdm: the display cannot create buffers\r\n");
        } else if (error != EBUSY) {
            printf("stlxdm: display buffer creation failed on output %u: %s\r\n", m_output, strerror(error));
        }

        return -error;
    }

    void* mem = mmap(nullptr, args.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, m_fd, static_cast<off_t>(args.map_offset));
    if (mem == MAP_FAILED) {
        int error = errno;
        printf("stlxdm: display buffer mapping failed on output %u: %s\r\n", m_output, strerror(error));
        ioctl(m_fd, gfxfb::GFXFB_DESTROY_BUFFER, static_cast<unsigned long>(args.handle));
        return -error;
    }

    buffer.handle = args.handle;
    buffer.pitch = args.pitch;
    buffer.size = args.size;
    buffer.pixels = static_cast<uint8_t*>(mem);

    return 0;
}

/* Unmaps and drops both display buffers and frees the composed frame. A buffer still on screen stays allocated
 * until the output shows another buffer or goes dark */
void flip_presenter::release_buffers() {
    free(m_composed);
    m_composed = nullptr;

    for (display_buffer& buffer : m_buffers) {
        if (buffer.pixels) {
            munmap(buffer.pixels, buffer.size);
        }

        if (buffer.handle != 0) {
            ioctl(m_fd, gfxfb::GFXFB_DESTROY_BUFFER, static_cast<unsigned long>(buffer.handle));
        }

        buffer = display_buffer();
    }
}

void flip_presenter::shutdown() {
    release_buffers();
    stop_cursor();
    m_fd = -1;
}

presenter::target flip_presenter::acquire() {
    m_frame_start_ns = now_ns();

    target t;
    t.pixels = reinterpret_cast<uint32_t*>(m_composed);
    t.stride = m_width * BYTES_PER_PIXEL;

    /* Composition reads back the previous frame and display memory can be slow to read, so
     * composition targets one cached buffer, which holds that frame from the second acquire on */
    t.age = m_first_acquire ? 0 : 1;
    m_first_acquire = false;

    return t;
}

void flip_presenter::present(const damage_list& damage) {
    wait_for_present();

    for (uint32_t i = 0; i < BUFFER_COUNT; i++) {
        note_changed(i, damage);
    }

    display_buffer& back = m_buffers[m_back_index];
    damage_list& changed = m_unpresented[m_back_index];
    if (changed.empty() || m_output_changed) {
        return;
    }

    if (changed.full()) {
        damage_list::rect whole = { 0, 0, static_cast<int32_t>(m_width),
                                    static_cast<int32_t>(m_height) };
        copy_rect(back.pixels, back.pitch, m_composed, m_width, whole);
    } else {
        for (uint32_t i = 0; i < changed.count(); i++) {
            copy_rect(back.pixels, back.pitch, m_composed, m_width, changed.at(i));
        }
    }

    present_back(back.handle);
}

/* Presents the buffer `handle` names with `changed`'s regions, all of it when it is full. Returns whether the display
 * accepted it, printing a line when presents start failing but not for an output gone dark or relit at another size */
bool buffer_presenter::present_buffer(uint32_t handle, const damage_list& changed) {
    gfxfb::gfxfb_rect regions[damage_list::MAX_RECTS];
    uint32_t region_count = 0;
    if (!changed.full()) {
        for (uint32_t i = 0; i < changed.count(); i++) {
            regions[region_count++] = to_gfxfb_rect(changed.at(i));
        }
    }

    gfxfb::gfxfb_present_args args = {};
    args.output = m_output;
    args.handle = handle;
    args.regions = reinterpret_cast<uint64_t>(regions);
    args.region_count = region_count;

    bool failed = ioctl(m_fd, gfxfb::GFXFB_PRESENT, &args) < 0;
    int error = errno;

    /* An output gone dark or relit at another size refuses with `EAGAIN` or `EINVAL` before the caller follows its
     * output event, so `EINVAL` counts as a failure unless the output reads dark or lit at another size */
    bool output_dark = failed && error == EAGAIN;
    bool size_changed = failed && error == EINVAL && !still_lit_at(m_fd, m_output, m_width, m_height);
    bool refused_by_change = output_dark || size_changed;
    if (failed && !refused_by_change && !m_last_present_failed) {
        printf("stlxdm: display present failed on output %u: %s\r\n", m_output, strerror(error));
    }

    m_last_present_failed = failed && !refused_by_change;

    return !failed;
}

/* A latched present put the back buffer on screen, so the other one becomes the back buffer. A present that
 * ended dark prints nothing, since its output's change also queues an output event the caller follows */
void buffer_presenter::end_present(uint32_t status, uint64_t frame, uint64_t latch_ns) {
    m_present_pending = false;

    if (status != gfxfb::GFXFB_PRESENT_LATCHED) {
        if (status == gfxfb::GFXFB_PRESENT_FAILED) {
            printf("stlxdm: a present ended without latching on output %u: the display failed\r\n", m_output);
        }

        /* An output that went dark counts its vertical blanks from 0 once lit again, so the next
         * latch counts no misses. A failed display latches no later present */
        m_has_latch = false;
        return;
    }

    count_missed_frames(frame, latch_ns);
    m_back_index = (m_back_index + 1) % BUFFER_COUNT;
}

void buffer_presenter::abandon_present() {
    m_present_pending = false;
    m_has_latch = false;
}

/* A relit output counts vertical blanks from 0 and shows its base buffer until a present latches */
void buffer_presenter::relit(const gfxfb::gfxfb_output_info& info) {
    m_refresh_millihertz = info.current.refresh_millihertz;
    m_cursor_max_width = info.cursor_max_width;
    m_cursor_max_height = info.cursor_max_height;
    m_has_latch = false;
    m_last_present_failed = false;
    m_output_changed = false;

    for (damage_list& unpresented : m_unpresented) {
        unpresented.add_full();
    }
}

/* Adds the vertical blanks the latched present missed and keeps its latch for the next, taking those
 * between two latches as evenly spaced. Event times and acquire()'s stamps share the monotonic clock */
void buffer_presenter::count_missed_frames(uint64_t frame, uint64_t latch_ns) {
    bool has_earlier_latch = m_has_latch && frame > m_latch_frame && latch_ns > m_latch_ns;
    uint64_t period_ns = has_earlier_latch ? (latch_ns - m_latch_ns) / (frame - m_latch_frame) : 0;
    if (period_ns != 0) {
        uint64_t since_latch_ns = m_pending_frame_start_ns > m_latch_ns ? m_pending_frame_start_ns - m_latch_ns : 0;
        uint64_t due_frame = m_latch_frame + (since_latch_ns + period_ns / 2) / period_ns + 1;
        if (frame > due_frame) {
            m_missed_frames += frame - due_frame;
        }
    }

    m_has_latch = true;
    m_latch_frame = frame;
    m_latch_ns = latch_ns;
}

bool buffer_presenter::set_cursor(const cursor_picture& picture) {
    bool fits = picture.width <= m_cursor_max_width && picture.height <= m_cursor_max_height;
    if (!has_cursor() || !fits) {
        return false;
    }

    gfxfb::gfxfb_cursor_args args = {};
    args.output = m_output;
    args.width = picture.width;
    args.height = picture.height;
    args.hot_x = picture.hot_x;
    args.hot_y = picture.hot_y;
    args.pixels = reinterpret_cast<uint64_t>(picture.pixels);

    bool taken = cursor_call(gfxfb::GFXFB_SET_CURSOR, &args, "set");
    if (taken) {
        m_cursor_failing = false;
    }

    return taken;
}

void buffer_presenter::hide_cursor() {
    if (!has_cursor()) {
        return;
    }

    gfxfb::gfxfb_cursor_args args = {};
    args.output = m_output;

    cursor_call(gfxfb::GFXFB_SET_CURSOR, &args, "hide");
}

bool buffer_presenter::move_cursor(int32_t x, int32_t y) {
    if (!has_cursor()) {
        return false;
    }

    gfxfb::gfxfb_cursor_position position = {};
    position.output = m_output;
    position.x = x;
    position.y = y;

    return cursor_call(gfxfb::GFXFB_MOVE_CURSOR, &position, "move");
}

/* Makes the cursor ioctl `request` and returns whether the display took it, naming `call` in a line at a failure while
 * `m_cursor_failing` is clear, but not for a dark output, which an output event reports */
bool buffer_presenter::cursor_call(uint32_t request, void* args, const char* call) {
    bool failed = ioctl(m_fd, request, args) < 0;
    bool output_dark = failed && errno == EAGAIN;
    if (failed && !output_dark && !m_cursor_failing) {
        printf("stlxdm: display cursor %s failed on output %u: %s\r\n",
               call, m_output, strerror(errno));
        m_cursor_failing = true;
    }

    return !failed;
}

int gpu_presenter::init(display& owner, const gfxfb::gfxfb_output_info& info, output_buffer_maker& maker) {
    take_mode(owner, info);

    for (std::unique_ptr<output_buffer>& buffer : m_buffers) {
        buffer = maker.make_output_buffer(m_fd, m_output, m_width, m_height);
        if (!buffer) {
            shutdown();
            return -1;
        }
    }

    for (damage_list& unpresented : m_unpresented) {
        unpresented.add_full();
    }

    take_cursor_limits(info);

    return 0;
}

/* A buffer still on screen stays allocated until the output shows another buffer or goes dark */
void gpu_presenter::shutdown() {
    for (std::unique_ptr<output_buffer>& buffer : m_buffers) {
        buffer.reset();
    }

    stop_cursor();
    m_fd = -1;
}

void gpu_presenter::relit(const gfxfb::gfxfb_output_info& info) {
    buffer_presenter::relit(info);

    for (uint64_t& frame : m_buffer_frames) {
        frame = 0;
    }
}

presenter::target gpu_presenter::acquire() {
    m_frame_start_ns = now_ns();
    m_frames++;

    const output_buffer& back = *m_buffers[m_back_index];
    uint64_t last_frame = m_buffer_frames[m_back_index];
    m_buffer_frames[m_back_index] = m_frames;

    target t;
    t.stride = back.pitch();
    t.age = last_frame == 0 ? 0 : static_cast<uint32_t>(m_frames - last_frame);
    t.gpu_address = back.gpu_address();

    return t;
}

void gpu_presenter::present(const damage_list& damage) {
    wait_for_present();
    note_changed(m_back_index, damage);

    if (m_unpresented[m_back_index].empty() || m_output_changed) {
        return;
    }

    present_back(m_buffers[m_back_index]->gfxfb_handle());
}
