#include "presenter.hpp"

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

/* Fills `primary` with the primary output's state. Returns 0, or a negative errno
 * after printing why: GFXFB_GET_OUTPUT's, or -EAGAIN when no output is primary */
static int read_primary_output(int fd, gfxfb::gfxfb_output_info& primary) {
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
            return 0;
        }

        count = primary.output_count;
    }

    printf("stlxdm: no primary output\r\n");

    return -EAGAIN;
}

/* The primary output's refresh rate, or 0 when it is unknown, no output is
 * primary or GFXFB_GET_OUTPUT fails, with a line printed for the last two */
static uint64_t read_primary_refresh(int fd) {
    gfxfb::gfxfb_output_info primary;
    if (read_primary_output(fd, primary) != 0) {
        return 0;
    }

    return primary.current.refresh_millihertz;
}

int memcpy_presenter::init() {
    m_fd = open("/dev/gfxfb", O_RDWR);
    if (m_fd < 0) {
        return -1;
    }

    gfxfb::gfxfb_info info;
    if (ioctl(m_fd, gfxfb::GFXFB_GET_INFO, &info) < 0) {
        close(m_fd);
        m_fd = -1;
        return -1;
    }

    /* The scanout mapping may be write-combining, where reads are
     * pathologically slow, so composition targets m_back instead */
    void* mem = mmap(nullptr, info.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, m_fd, 0);
    if (mem == MAP_FAILED) {
        close(m_fd);
        m_fd = -1;
        return -1;
    }

    m_width = static_cast<uint32_t>(info.width);
    m_height = static_cast<uint32_t>(info.height);
    m_pitch = static_cast<uint32_t>(info.pitch);
    m_size = info.size;
    m_scanout = static_cast<uint8_t*>(mem);
    m_needs_flush = (info.flags & gfxfb::GFXFB_INFO_NEEDS_FLUSH) != 0;
    m_refresh_millihertz = read_primary_refresh(m_fd);

    m_back = static_cast<uint8_t*>(
        malloc(static_cast<size_t>(m_width) * m_height * 4));
    if (!m_back) {
        shutdown();
        return -1;
    }

    return 0;
}

void memcpy_presenter::shutdown() {
    free(m_back);
    m_back = nullptr;

    if (m_scanout) {
        munmap(m_scanout, m_size);
        m_scanout = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
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

    bool failed = ioctl(m_fd, gfxfb::GFXFB_FLUSH, &args) < 0;
    if (failed && !m_last_flush_failed) {
        printf("stlxdm: display flush failed: %s\r\n", strerror(errno));
    }

    m_last_flush_failed = failed;
}

void memcpy_presenter::present(const damage_list& damage) {
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

int flip_presenter::init() {
    m_fd = open("/dev/gfxfb", O_RDWR);
    if (m_fd < 0) {
        int error = errno;
        printf("stlxdm: display open failed: %s\r\n", strerror(error));
        return -error;
    }

    gfxfb::gfxfb_output_info primary;
    int rc = read_primary_output(m_fd, primary);
    if (rc != 0) {
        shutdown();
        return rc;
    }

    m_output = primary.output;
    m_width = primary.current.width;
    m_height = primary.current.height;
    m_refresh_millihertz = primary.current.refresh_millihertz;
    m_cursor_max_width = primary.cursor_max_width;
    m_cursor_max_height = primary.cursor_max_height;

    for (display_buffer& buffer : m_buffers) {
        rc = create_buffer(buffer);
        if (rc != 0) {
            shutdown();
            return rc;
        }
    }

    m_composed = static_cast<uint8_t*>(
        malloc(static_cast<size_t>(m_width) * m_height * BYTES_PER_PIXEL));
    if (!m_composed) {
        printf("stlxdm: no memory for the composed frame\r\n");
        shutdown();
        return -ENOMEM;
    }

    for (damage_list& unpresented : m_unpresented) {
        unpresented.add_full();
    }

    return 0;
}

/* Creates one buffer at the primary output's mode and maps it. Returns 0 or a negative errno,
 * printing why for all but -EBUSY. A buffer left unmapped stays with the file until it closes */
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
            printf("stlxdm: display buffer creation failed: %s\r\n", strerror(error));
        }

        return -error;
    }

    void* mem = mmap(nullptr, args.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, m_fd, static_cast<off_t>(args.map_offset));
    if (mem == MAP_FAILED) {
        int error = errno;
        printf("stlxdm: display buffer mapping failed: %s\r\n", strerror(error));
        return -error;
    }

    buffer.handle = args.handle;
    buffer.pitch = args.pitch;
    buffer.size = args.size;
    buffer.pixels = static_cast<uint8_t*>(mem);

    return 0;
}

void flip_presenter::shutdown() {
    free(m_composed);
    m_composed = nullptr;

    for (display_buffer& buffer : m_buffers) {
        if (buffer.pixels) {
            munmap(buffer.pixels, buffer.size);
        }

        buffer = display_buffer();
    }

    /* Closing drops every buffer handle, and a file in control presents the outputs' base buffers again */
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
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
    while (m_present_pending) {
        take_end();
    }

    for (damage_list& unpresented : m_unpresented) {
        if (damage.full()) {
            unpresented.add_full();
            continue;
        }

        for (uint32_t i = 0; i < damage.count(); i++) {
            damage_list::rect r = damage.at(i);
            if (damage_list::clip(r, static_cast<int32_t>(m_width), static_cast<int32_t>(m_height))) {
                unpresented.add(r.x, r.y, r.w, r.h);
            }
        }
    }

    display_buffer& back = m_buffers[m_back_index];
    damage_list& changed = m_unpresented[m_back_index];
    if (changed.empty()) {
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

    if (!present_buffer(back, changed)) {
        return;
    }

    changed.clear();
    m_present_pending = true;
    m_pending_frame_start_ns = m_frame_start_ns;
}

/* Presents `buffer` with the regions in `changed`, all of it when `changed` is full. Returns
 * whether the display accepted it, printing a line when presents start failing */
bool flip_presenter::present_buffer(const display_buffer& buffer, const damage_list& changed) {
    gfxfb::gfxfb_rect regions[damage_list::MAX_RECTS];
    uint32_t region_count = 0;
    if (!changed.full()) {
        for (uint32_t i = 0; i < changed.count(); i++) {
            regions[region_count++] = to_gfxfb_rect(changed.at(i));
        }
    }

    gfxfb::gfxfb_present_args args = {};
    args.output = m_output;
    args.handle = buffer.handle;
    args.regions = reinterpret_cast<uint64_t>(regions);
    args.region_count = region_count;

    bool failed = ioctl(m_fd, gfxfb::GFXFB_PRESENT, &args) < 0;
    if (failed && !m_last_present_failed) {
        printf("stlxdm: display present failed: %s\r\n", strerror(errno));
    }

    m_last_present_failed = failed;

    return !failed;
}

/* A latched present put the back buffer on screen, so the other one becomes the back
 * buffer. A read failing for any reason but a signal stops the wait rather than retrying */
void flip_presenter::take_end() {
    gfxfb::gfxfb_event end;
    if (read(m_fd, &end, sizeof(end)) < 0) {
        if (errno != EINTR) {
            printf("stlxdm: display event read failed: %s\r\n", strerror(errno));
            m_present_pending = false;
            m_has_latch = false;
        }

        return;
    }

    m_present_pending = false;

    if (end.status != gfxfb::GFXFB_PRESENT_LATCHED) {
        printf("stlxdm: a present ended without latching: %s\r\n",
               end.status == gfxfb::GFXFB_PRESENT_DARK ? "the output went dark" : "the display failed");

        /* An output that went dark counts its vertical blanks from 0 once lit again, so the next
         * latch counts no misses. A failed display latches no later present */
        m_has_latch = false;
        return;
    }

    count_missed_frames(end.frame, end.timestamp_ns);
    m_back_index = (m_back_index + 1) % BUFFER_COUNT;
}

/* Adds the vertical blanks the latched present missed and keeps its latch for the next, taking those
 * between two latches as evenly spaced. Event times and acquire()'s stamps share the monotonic clock */
void flip_presenter::count_missed_frames(uint64_t frame, uint64_t latch_ns) {
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

bool flip_presenter::set_cursor(const cursor_picture& picture) {
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

void flip_presenter::hide_cursor() {
    if (!has_cursor()) {
        return;
    }

    gfxfb::gfxfb_cursor_args args = {};
    args.output = m_output;

    cursor_call(gfxfb::GFXFB_SET_CURSOR, &args, "hide");
}

bool flip_presenter::move_cursor(int32_t x, int32_t y) {
    if (!has_cursor()) {
        return false;
    }

    gfxfb::gfxfb_cursor_position position = {};
    position.output = m_output;
    position.x = x;
    position.y = y;

    return cursor_call(gfxfb::GFXFB_MOVE_CURSOR, &position, "move");
}

/* Makes the cursor ioctl `request` and returns whether the display took it,
 * naming `call` in a line at a failure while `m_cursor_failing` is clear */
bool flip_presenter::cursor_call(uint32_t request, void* args, const char* call) {
    bool failed = ioctl(m_fd, request, args) < 0;
    if (failed && !m_cursor_failing) {
        printf("stlxdm: display cursor %s failed: %s, pointer drawn into frames\r\n",
               call, strerror(errno));
        m_cursor_failing = true;
    }

    return !failed;
}
